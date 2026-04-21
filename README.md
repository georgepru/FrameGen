# FrameGen

Real-time AI frame doubling for capture card gaming. Captures HDMI input, uses a RIFE neural network to interpolate new frames, and presents the result fullscreen at 2× the source frame rate — all on your GPU with no game modifications required.

> **Note:** This repo includes a precompiled `framegen_mvp.exe`. The full source is also here if you want to build it yourself.

## Requirements

- Windows 10 22H2+ (x64)
- A capture card (Elgato 4K X USB or 4K Pro PCIe tested; other DirectShow/MF cards may work)
- A GPU with DirectML support — NVIDIA RTX 30xx / 40xx recommended
- The game or source running on a **separate PC or console** outputting HDMI to the capture card

## Quick Start

1. **Download** `FrameGen_v1.0.zip` from the [Releases page](https://github.com/georgepru/FrameGen/releases) and extract it
2. **Run** `launcher.exe`
3. **Benchmark** — click **Run Benchmark (experimental)** and wait for a recommended settings popup
4. **Configure** — pick your capture device, ONNX model, and any upscale/audio options
5. **Launch** — hit **Launch**; a fullscreen interpolated output window opens at 2× your source frame rate
6. Press **Q** or **Esc** to close the program

> ⚠️ This is an experimental project. There are known issues — see the [Issues page](https://github.com/georgepru/FrameGen/issues).

## Building from source

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Requires Visual Studio 2022 with the C++ Desktop workload, Windows SDK 10.0.22621+, and the ONNX Runtime DirectML package in `deps/onnxruntime/`.

## How it works

```
HDMI → Capture card → Media Foundation
     → BGRA texture (D3D12)
     → RIFE neural net (ONNX Runtime + DirectML)
     → 2× frame rate output (DXGI flip-discard, tearing-allowed)
```

## License

MIT — see [LICENSE](LICENSE).
