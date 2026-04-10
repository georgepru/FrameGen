# Frame Generation MVP

Live HDMI capture → RIFE frame interpolation → fullscreen display. Proves the pipeline latency claim from the MVP document.

## Requirements

- Windows 10 22H2+ (x64)
- Visual Studio 2022 with C++ Desktop workload
- Windows SDK 10.0.22621+
- Elgato 4K X (USB) or Elgato 4K Pro (PCIe) capture card
- GPU with D3D12 feature level 12.0 + DirectML support (RTX 30xx/40xx recommended)
- ONNX Runtime 1.24+ with DirectML EP — see **Setup** below

## Setup

### 1. ONNX Runtime

Download the DirectML zip from the [ORT releases page](https://github.com/microsoft/onnxruntime/releases):

```
onnxruntime-win-x64-directml-1.24.4.zip
```

Extract into `deps/onnxruntime/`. The expected layout after extraction:

```
deps/onnxruntime/
  include/onnxruntime_cxx_api.h
  include/dml_provider_factory.h
  lib/onnxruntime.lib
  lib/onnxruntime.dll
  lib/onnxruntime_providers_shared.dll
```

### 2. RIFE model

Export the RIFE model to ONNX using the existing `preview_cpp/export_onnx.py` script, or copy `rife_hd.onnx` from `preview_cpp/` and rename it to `rife.onnx` in the build output dir.

## Build

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build\Release\framegen_mvp.exe`

## Run

```
framegen_mvp.exe [rife.onnx] [deviceIndex]
```

- `rife.onnx` defaults to the file in the same directory as the exe.
- `deviceIndex` defaults to `0` (first capture card).

### Key bindings

| Key | Action |
|-----|--------|
| `I` | Toggle RIFE interpolation on/off |
| `D` | Print capture device list to stdout |
| `Q` / `Esc` | Quit |

## Architecture

```
HDMI → Elgato 4K X (USB)
     → Media Foundation Source Reader (MF_LOW_LATENCY, D3D11 GPU path)
     → CapturedFrame (BGRA D3D12 texture via D3D11On12)
     → TextureConverter (BGRA → NCHW float32, compute shader)
     → RifeInference (ONNX Runtime + DirectML EP, zero-copy DML tensors)
     → SwapPresenter (DXGI flip-discard, waitable object, tearing-allowed)
     → Fullscreen display
```

### Threads

| Thread | Role |
|--------|------|
| CaptureThread | Keeps MF alive; frames arrive via MF async callback |
| RifeThread | Pairs frames, runs BGRA→NCHW + RIFE, enqueues present |
| PresentThread | Dequeues and calls SwapPresenter::Present |

### Latency controls

- `MF_LOW_LATENCY = TRUE` on the source reader reduces MF internal buffering.
- `FrameQueue` depth of 3 (capture) and 2 (present) — stale frames are dropped.
- `SetMaximumFrameLatency(1)` + waitable object on the swap chain.
- `DXGI_PRESENT_ALLOW_TEARING` = immediate present, no vsync wait.
- Dedicated `dmlQueue12` for ORT/DML, isolated from D3D11On12 to prevent TDR.

## Telemetry

A CSV log (`framegen_log.csv`, next to the exe) is written on exit with per-frame timing:

| Column | Description |
|--------|-------------|
| `captureArrivalUs` | MF callback → queue push, microseconds from first frame |
| `rifeStartUs` | inference start |
| `rifeEndUs` | inference end |
| `presentedUs` | Present() called |
| `inferenceMs` | RIFE duration in milliseconds |
| `dropped` | 1 = frame was dropped from the queue |

Live stats are printed to stdout (input FPS, output FPS, RIFE ms, dropped frames).

## Known limitations (MVP scope)

- No audio pass-through.
- No recording or stream output.
- Single capture card only.
- Pass-through mode (I key off) does not blit the BGRA frame — this is a Phase 4 polish item.
- Swap chain resolution is locked to the capture card's native output; window resize is not handled.
