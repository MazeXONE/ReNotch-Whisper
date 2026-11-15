# ReNotch Whisper

The speech-to-text engine behind **ReNotch Typing Voice**. It runs a Whisper `ggml-*.bin`
speech model and turns a recording into text. Built from
[whisper.cpp](https://github.com/ggml-org/whisper.cpp) v1.9.5.

ReNotch downloads this engine from the Releases page the first time a user downloads a speech
model, so the ReNotch installer stays small.

## Downloads

| File | Use |
|---|---|
| `renotch-whisper-cpu.exe` | Every 64-bit Windows 10/11 PC (AVX2 CPU). |
| `renotch-whisper-vulkan.exe` | GPU through Vulkan (AMD, NVIDIA, Intel); falls back to CPU by itself. |
| `SHA256SUMS.txt` | SHA-256 of both files. |

Each is one self-contained `.exe`: static MSVC runtime, no OpenMP DLL, no ggml/whisper DLLs.

Stable URLs for the app:

```
https://github.com/MazeXONE/ReNotch-Whisper/releases/download/v1.0.0/renotch-whisper-cpu.exe
https://github.com/MazeXONE/ReNotch-Whisper/releases/download/v1.0.0/renotch-whisper-vulkan.exe
https://github.com/MazeXONE/ReNotch-Whisper/releases/download/v1.0.0/SHA256SUMS.txt
```

Use a pinned tag (not `releases/latest`) so a future engine release can never change what an
already-shipped ReNotch build talks to.

## Where ReNotch keeps it

```
%LOCALAPPDATA%\ReNotch\engines\renotch-whisper-cpu.exe
%LOCALAPPDATA%\ReNotch\engines\renotch-whisper-vulkan.exe
%LOCALAPPDATA%\ReNotch\models\ggml-*.bin
```

## Protocol

One JSON object per line on **stdin**, one JSON object per line on **stdout**. The process
exits when stdin is closed. Paths are UTF-8; non-ASCII paths work.

```jsonc
// load (or reuse) a model; gpu:true tries Vulkan first and falls back to CPU
-> {"cmd":"load","model":"C:\\Users\\me\\AppData\\Local\\ReNotch\\models\\ggml-large-v3-turbo.bin","gpu":true}
<- {"ok":true,"event":"loaded","gpu":true}

// transcribe raw float32 mono 16 kHz PCM; lang is an ISO code or "auto"
-> {"cmd":"transcribe","pcm":"C:\\...\\renotch_lazytype_pcm.f32","lang":"id","prompt":"...","beam":false}
<- {"ok":true,"text":"halo, ini hasil ketikan suara"}

-> {"cmd":"version"}
<- {"ok":true,"engine":"renotch-whisper","version":"1.0.0","whisper":"v1.9.5","backend":"vulkan"}
```

Errors come back as `{"ok":false,"error":"..."}`. A failed `transcribe` also carries
`"text":""`, so a caller waiting for a `"text"` line never hangs. Silent or very short
recordings return `"text":""` instead of a Whisper hallucination. Logs are switched off, so
stdout only ever carries protocol lines (safe when stderr is merged into the same pipe).

`renotch-whisper-*.exe --version` prints the version line and exits.

## Build

Builds run on GitHub Actions (`.github/workflows/build.yml`) for every push. Pushing a tag
`vX.Y.Z` (matching `project(VERSION ...)` in `CMakeLists.txt`) builds both variants, runs a real
transcription test with the tiny model, and publishes the Release.

Locally (Visual Studio 2022 + CMake; Vulkan SDK for the GPU build):

```
cmake -S . -B build -A x64 -DRENOTCH_WHISPER_VULKAN=ON
cmake --build build --config Release --target renotch-whisper
```

## License

MIT. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
