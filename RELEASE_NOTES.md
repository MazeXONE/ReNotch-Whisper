Speech-to-text engine for **ReNotch Typing Voice**, built from whisper.cpp v1.9.5.

| File | Use |
|---|---|
| `renotch-whisper-cpu.exe` | Works on every 64-bit Windows 10/11 PC (AVX2 CPU). |
| `renotch-whisper-vulkan.exe` | Uses the GPU through Vulkan (AMD, NVIDIA, Intel). Falls back to CPU if the GPU can't be used. |
| `SHA256SUMS.txt` | Checksums to verify the downloads. |

Both are single self-contained `.exe` files: no installer, no VC++ Redistributable, no extra DLLs. ReNotch downloads them automatically; you don't need to download them by hand.
