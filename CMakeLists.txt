cmake_minimum_required(VERSION 3.21)

# Static MSVC runtime: the .exe must run without the VC++ Redistributable.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)

project(renotch-whisper VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(RENOTCH_WHISPER_VULKAN "Build the Vulkan (GPU) variant" OFF)

set(WHISPER_UPSTREAM_TAG    "v1.9.5")
set(WHISPER_UPSTREAM_COMMIT "d1be6fde11ac6e0407606b4e42fe72d34add8037")

# One self-contained .exe: no shared ggml/whisper DLLs, no OpenMP runtime DLL.
set(BUILD_SHARED_LIBS        OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER     OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP              OFF CACHE BOOL "" FORCE)
set(GGML_BACKEND_DL          OFF CACHE BOOL "" FORCE)
# Portable CPU code (AVX2/FMA/F16C works on practically every PC since 2013).
set(GGML_NATIVE              OFF CACHE BOOL "" FORCE)
set(GGML_AVX                 ON  CACHE BOOL "" FORCE)
set(GGML_AVX2                ON  CACHE BOOL "" FORCE)
set(GGML_FMA                 ON  CACHE BOOL "" FORCE)
set(GGML_F16C                ON  CACHE BOOL "" FORCE)
set(GGML_VULKAN ${RENOTCH_WHISPER_VULKAN} CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(whisper
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG        ${WHISPER_UPSTREAM_COMMIT}
    GIT_SHALLOW    FALSE)
FetchContent_MakeAvailable(whisper)

if(RENOTCH_WHISPER_VULKAN)
    set(_backend "vulkan")
else()
    set(_backend "cpu")
endif()

add_executable(renotch-whisper src/main.cpp)
set_target_properties(renotch-whisper PROPERTIES OUTPUT_NAME "renotch-whisper-${_backend}")
target_link_libraries(renotch-whisper PRIVATE whisper)
target_compile_definitions(renotch-whisper PRIVATE
    RENOTCH_WHISPER_VERSION="${PROJECT_VERSION}"
    RENOTCH_WHISPER_UPSTREAM="${WHISPER_UPSTREAM_TAG}"
    RENOTCH_WHISPER_BACKEND="${_backend}")
if(MSVC)
    target_compile_options(renotch-whisper PRIVATE /utf-8 /W3)
endif()
