// renotch-whisper — speech-to-text sidecar engine for ReNotch Typing Voice.
//
// Protocol (one JSON object per line on stdin, one JSON object per line on stdout):
//
//   -> {"cmd":"load","model":"C:\\...\\ggml-large-v3-turbo.bin","gpu":true}
//   <- {"ok":true,"event":"loaded","gpu":true}
//
//   -> {"cmd":"transcribe","pcm":"C:\\...\\renotch_lazytype_pcm.f32","lang":"id","prompt":"...","beam":false}
//   <- {"ok":true,"text":"hasil ketikan suara"}
//
//   -> {"cmd":"version"}
//   <- {"ok":true,"engine":"renotch-whisper","version":"1.0.0","whisper":"1.9.5","backend":"vulkan"}
//
// The PCM file is raw little-endian float32, mono, 16 kHz (what ReNotch records).
// Failures are reported as {"ok":false,"error":"..."} and never as a "text" line,
// except a failed transcribe, which also answers {"ok":false,"error":"...","text":""}
// so the caller stops waiting immediately.
//
// The process exits when stdin reaches EOF.

#include "whisper.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#ifndef RENOTCH_WHISPER_VERSION
#define RENOTCH_WHISPER_VERSION "1.0.0"
#endif
#ifndef RENOTCH_WHISPER_UPSTREAM
#define RENOTCH_WHISPER_UPSTREAM "unknown"
#endif
#ifndef RENOTCH_WHISPER_BACKEND
#define RENOTCH_WHISPER_BACKEND "cpu"
#endif

namespace {

constexpr int kSampleRate = 16000;

// ---------------------------------------------------------------- JSON (tiny)

// Finds "key": in a single-line JSON object and returns the position right after the colon.
std::size_t find_value_start(std::string_view line, std::string_view key) {
    std::string needle;
    needle.reserve(key.size() + 2);
    needle.push_back('"');
    needle.append(key);
    needle.push_back('"');
    std::size_t pos = 0;
    while ((pos = line.find(needle, pos)) != std::string_view::npos) {
        std::size_t p = pos + needle.size();
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
        if (p < line.size() && line[p] == ':') {
            ++p;
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
            return p;
        }
        pos += needle.size();
    }
    return std::string_view::npos;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool parse_hex4(std::string_view s, std::size_t at, uint32_t& v) {
    if (at + 4 > s.size()) return false;
    v = 0;
    for (std::size_t i = at; i < at + 4; ++i) {
        const char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
        else return false;
    }
    return true;
}

bool json_get_string(std::string_view line, std::string_view key, std::string& out) {
    std::size_t p = find_value_start(line, key);
    if (p == std::string_view::npos || p >= line.size() || line[p] != '"') return false;
    out.clear();
    for (++p; p < line.size(); ++p) {
        const char c = line[p];
        if (c == '"') return true;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (++p >= line.size()) return false;
        switch (line[p]) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!parse_hex4(line, p + 1, cp)) return false;
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 < line.size() && line[p + 1] == '\\' && line[p + 2] == 'u') {
                    uint32_t lo = 0;
                    if (parse_hex4(line, p + 3, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                append_utf8(out, cp);
                break;
            }
            default: out.push_back(line[p]); break;  // \" \\ \/
        }
    }
    return false;
}

bool json_get_bool(std::string_view line, std::string_view key, bool fallback) {
    const std::size_t p = find_value_start(line, key);
    if (p == std::string_view::npos) return fallback;
    if (line.substr(p, 4) == "true") return true;
    if (line.substr(p, 5) == "false") return false;
    return fallback;
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) out.push_back(' ');
        else out.push_back(ch);  // UTF-8 bytes pass through unchanged
    }
    return out;
}

void emit(const std::string& line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void emit_error(const char* what, bool with_empty_text) {
    std::string line = "{\"ok\":false,\"error\":\"";
    line += json_escape(what);
    line += "\"";
    if (with_empty_text) line += ",\"text\":\"\"";
    line += "}";
    emit(line);
}

// ------------------------------------------------------------- file access

FILE* open_utf8_path(const std::string& path) {
#ifdef _WIN32
    const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return nullptr;
    std::wstring wpath(static_cast<std::size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
    return ::_wfopen(wpath.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

size_t loader_read(void* ctx, void* output, size_t read_size) {
    return std::fread(output, 1, read_size, static_cast<FILE*>(ctx));
}
bool loader_eof(void* ctx) {
    return std::feof(static_cast<FILE*>(ctx)) != 0;
}
void loader_close(void* ctx) {
    std::fclose(static_cast<FILE*>(ctx));
}

bool read_pcm_f32(const std::string& path, std::vector<float>& pcm) {
    FILE* f = open_utf8_path(path);
    if (!f) return false;
    pcm.clear();
    float buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, sizeof(float), 4096, f)) > 0) {
        pcm.insert(pcm.end(), buf, buf + n);
    }
    std::fclose(f);
    return true;
}

// ---------------------------------------------------------------- engine

void silent_log(enum ggml_log_level, const char*, void*) {}

struct Engine {
    whisper_context* ctx = nullptr;
    std::string model_path;
    bool gpu = false;

    ~Engine() { unload(); }

    void unload() {
        if (ctx) whisper_free(ctx);
        ctx = nullptr;
        model_path.clear();
    }

    whisper_context* try_load(const std::string& path, bool use_gpu) {
        FILE* f = open_utf8_path(path);
        if (!f) return nullptr;
        whisper_model_loader loader{};
        loader.context = f;
        loader.read = loader_read;
        loader.eof = loader_eof;
        loader.close = loader_close;
        whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu = use_gpu;
        cparams.flash_attn = use_gpu;
        // whisper_init_with_params closes the loader itself (success or failure).
        return whisper_init_with_params(&loader, cparams);
    }

    bool load(const std::string& path, bool want_gpu) {
        if (ctx && model_path == path && gpu == want_gpu) return true;
        unload();
        whisper_context* c = nullptr;
        bool used_gpu = false;
        if (want_gpu) {
            try {
                c = try_load(path, true);
            } catch (...) {
                c = nullptr;
            }
            used_gpu = (c != nullptr);
        }
        if (!c) {
            try {
                c = try_load(path, false);  // CPU fallback, always available
            } catch (...) {
                c = nullptr;
            }
            used_gpu = false;
        }
        if (!c) return false;
        ctx = c;
        model_path = path;
        gpu = used_gpu;
        return true;
    }
};

int pick_threads() {
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned physical_guess = hw > 1 ? hw / 2 : 1;
    return static_cast<int>(std::clamp<unsigned>(physical_guess, 2u, 8u));
}

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

void handle_transcribe(Engine& engine, std::string_view line) {
    if (!engine.ctx) {
        emit_error("model not loaded", true);
        return;
    }
    std::string pcm_path, lang, prompt;
    if (!json_get_string(line, "pcm", pcm_path) || pcm_path.empty()) {
        emit_error("missing pcm path", true);
        return;
    }
    if (!json_get_string(line, "lang", lang) || lang.empty()) lang = "auto";
    (void)json_get_string(line, "prompt", prompt);
    const bool beam = json_get_bool(line, "beam", false);

    std::vector<float> pcm;
    if (!read_pcm_f32(pcm_path, pcm)) {
        emit_error("cannot open pcm file", true);
        return;
    }

    // Too short or silent: answer empty instead of letting Whisper hallucinate.
    double energy = 0.0;
    for (const float s : pcm) energy += static_cast<double>(s) * s;
    const double rms = pcm.empty() ? 0.0 : std::sqrt(energy / static_cast<double>(pcm.size()));
    if (pcm.size() < static_cast<std::size_t>(kSampleRate / 4) || rms < 0.0015) {
        emit("{\"ok\":true,\"text\":\"\"}");
        return;
    }
    // Whisper refuses input shorter than 1 s; pad with silence.
    const std::size_t min_len = static_cast<std::size_t>(kSampleRate) * 11 / 10;
    if (pcm.size() < min_len) pcm.resize(min_len, 0.0f);

    whisper_full_params p = whisper_full_default_params(
        beam ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
    p.n_threads = pick_threads();
    p.language = lang.c_str();
    p.detect_language = false;  // true would only detect, not transcribe
    p.translate = false;
    p.no_context = true;
    p.no_timestamps = true;
    p.single_segment = false;
    p.print_special = false;
    p.print_progress = false;
    p.print_realtime = false;
    p.print_timestamps = false;
    p.suppress_blank = true;
    p.suppress_nst = true;
    p.initial_prompt = prompt.empty() ? nullptr : prompt.c_str();
    if (beam) p.beam_search.beam_size = 5;

    int rc = -1;
    try {
        rc = whisper_full(engine.ctx, p, pcm.data(), static_cast<int>(pcm.size()));
    } catch (...) {
        rc = -1;
    }
    if (rc != 0) {
        emit_error("transcription failed", true);
        return;
    }

    std::string text;
    const int n = whisper_full_n_segments(engine.ctx);
    for (int i = 0; i < n; ++i) {
        const char* seg = whisper_full_get_segment_text(engine.ctx, i);
        if (seg) text += seg;
    }
    emit("{\"ok\":true,\"text\":\"" + json_escape(trim(std::move(text))) + "\"}");
}

std::string version_line() {
    return std::string("{\"ok\":true,\"engine\":\"renotch-whisper\",\"version\":\"") + RENOTCH_WHISPER_VERSION +
           "\",\"whisper\":\"" + RENOTCH_WHISPER_UPSTREAM + "\",\"backend\":\"" + RENOTCH_WHISPER_BACKEND + "\"}";
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Never show loader / critical-error dialogs: a sidecar must fail quietly.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    ::SetDllDirectoryW(L"");  // drop the current directory from the DLL search path
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            emit(version_line());
            return 0;
        }
    }

    whisper_log_set(silent_log, nullptr);  // stderr is merged into the caller's stdout pipe

    Engine engine;
    std::string line;
    int ch = 0;
    for (;;) {
        line.clear();
        while ((ch = std::fgetc(stdin)) != EOF && ch != '\n') line.push_back(static_cast<char>(ch));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            std::string cmd;
            if (!json_get_string(line, "cmd", cmd)) {
                emit_error("missing cmd", false);
            } else if (cmd == "load") {
                std::string model;
                if (!json_get_string(line, "model", model) || model.empty()) {
                    emit_error("missing model path", false);
                } else if (engine.load(model, json_get_bool(line, "gpu", true))) {
                    emit(std::string("{\"ok\":true,\"event\":\"loaded\",\"gpu\":") +
                         (engine.gpu ? "true" : "false") + "}");
                } else {
                    emit_error("cannot load model", false);
                }
            } else if (cmd == "transcribe") {
                handle_transcribe(engine, line);
            } else if (cmd == "version") {
                emit(version_line());
            } else if (cmd == "unload") {
                engine.unload();
                emit("{\"ok\":true,\"event\":\"unloaded\"}");
            } else {
                emit_error("unknown cmd", false);
            }
        }
        if (ch == EOF) break;
    }
    return 0;
}
