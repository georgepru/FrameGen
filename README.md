# FrameGen

Real-time AI frame doubling for capture card gaming. Captures HDMI input, uses a RIFE neural network to interpolate new frames, and presents the result fullscreen at 2× the source frame rate — all on your GPU with no game modifications required.

> **Note:** This repo includes a precompiled `framegen_mvp.exe`. The full source is also here if you want to build it yourself.

## Requirements

- Windows 10 22H2+ (x64)
- A capture card (Elgato 4K X USB or 4K Pro PCIe tested; other DirectShow/MF cards may work)
- A GPU with DirectML support — NVIDIA RTX 30xx / 40xx recommended
- The game or source running on a **separate PC or console** outputting HDMI to the capture card

## Getting Started

### 1. Download

Grab the latest release from the [Releases page](https://github.com/georgepru/FrameGen/releases). You need:

- `launcher.exe` — the GUI launcher
- `framegen_mvp.exe` — the engine
- `onnxruntime.dll` + `onnxruntime_providers_shared.dll` — bundled, no install needed

Put all four files in the same folder.

### 2. First launch — download models

Run `launcher.exe`. On first launch it opens the **Setup** tab automatically.

Click **Download** next to the model that matches your capture resolution:
- **720p model** (~19 MB) — for 720p/1080i capture
- **1080p model** (~30 MB) — for 1080p capture

Wait for the download to finish, then click **Run Benchmark** to verify your GPU can handle real-time inference.

### 3. Configure and run

Switch to the **Main** tab:

1. Pick your capture device from the dropdown
2. Pick your ONNX model (720p or 1080p)
3. Toggle **No Audio** or **FSR upscale** if you want them
4. Hit **Launch**

A fullscreen window opens showing the interpolated output at 2× your source frame rate.

### Key bindings (while running)

| Key | Action |
|-----|--------|
| `I` | Toggle RIFE interpolation on/off |
| `Q` / `Esc` | Quit |

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
