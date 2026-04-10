# FrameGen — Continuation Notes
*Written April 10, 2026. Pick up here on the capture-card PC.*

---

## What This Project Is

Real-time HDMI frame interpolation using RIFE (AI 2x frame rate) via ONNX Runtime + DirectML.
Pipeline: Capture source → BGRA → GPU NCHW → RIFE inference → D3D12 present at 2x source FPS.

Stack: C++20 / MSVC / CMake, D3D12, D3D11On12, ONNX Runtime 1.24.4 + DirectML EP, FP16 tensors.

---

## Branch State

| Branch | Status | Description |
|--------|--------|-------------|
| `master` | stable | --compare mode, FP16, vsync fix |
| `feature/file-pacing` | ready to merge | decode-ahead buffer + 1ms timer resolution for smooth MP4 playback |

**`feature/file-pacing` has not been merged to master yet** — test it with the capture card first, then merge if happy.

---

## Prerequisites on This Machine

1. **Visual Studio 2022** with "Desktop development with C++" workload
2. **CMake** (included with VS, or install separately and add to PATH)
3. All ONNX Runtime binaries are tracked in git (`deps/onnxruntime/lib/`) — no manual copy needed
4. `rife.onnx` — **NOT in the repo**, copy it manually to `build\Release\rife.onnx` after building (see below).
   This is **RIFE HDv3, FP16 export** — the file is `RIFEtest\preview_cpp\rife_fp16.onnx` (4,676,160 bytes).
   It was exported via `RIFEtest\preview_cpp\export_onnx.py` from the HDv3 model weights.

---

## Clone and Build

```powershell
git clone https://github.com/georgepru/FrameGen.git
cd FrameGen

# To work on the pacing branch (recommended — it's the latest work):
git checkout feature/file-pacing

# Configure and build
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -B build -G "Visual Studio 17 2022" -A x64
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

After build succeeds, copy `rife.onnx` to `build\Release\rife.onnx`.

---

## Running (File Mode — MP4 test)

```powershell
cd build\Release
.\framegen_mvp.exe --file "C:\path\to\video.mp4"

# Side-by-side compare (interpolated left, original right):
.\framegen_mvp.exe --file "C:\path\to\video.mp4" --compare
```

Keys: `Q` / `Esc` = quit, `I` = toggle RIFE interpolation on/off.

---

## Running (Capture Card Mode)

```powershell
cd build\Release

# List available capture devices:
.\framegen_mvp.exe

# Use device 0 (default):
.\framegen_mvp.exe rife.onnx 0
```

If you have multiple capture devices, pass the index (0, 1, 2...).

---

## Capture Card Integration — What Needs Doing

The `CaptureSource` class already exists (`src/CaptureSource.cpp/.h`) and uses
Media Foundation `IMFSourceReader` to read from a video capture device.
It was built alongside the file mode path and should work with the Elgato 4K X.

**First thing to do when the card arrives:**
1. Run without `--file` to enumerate devices and confirm the card is visible
2. Check what format/resolution it advertises (720p30 expected → NV12 or BGRA)
3. If NV12: the `NV12toRGBA.hlsl` shader is already in the project and wired in
4. Run with the card as source, verify frames flow through RIFE, check overlay FPS/ms stats

**Expected latency:** ~1 source frame (33ms at 30fps) — irreducible minimum for RIFE 2x.

---

## Key Architecture Notes

- **Two GPU command queues:** `cmdQueue12` (present/11On12), `dmlQueue12` (ORT/DirectML)
- **FP16 everywhere:** tensors, UAVs, SRVs all use R16_FLOAT / min16float
- **SyncInterval=1 is required** — tearing mode (SyncInterval=0) caused FLIP_DISCARD to silently drop interpolated frames
- **compare mode** (`--compare`): single fullscreen window, interpolated left half, original BGRA right half — useful for evaluating quality
- **Overlay disabled in compare mode** (swapchain is full-screen sized, not padded-video sized)
- **timeBeginPeriod(1)** is called at startup for 1ms sleep timer resolution — critical for frame pacing accuracy

---

## What Was Fixed / Why It Works

| Problem | Fix |
|---------|-----|
| ORT refused FP16 model | Changed tensors to FLOAT16 element type, uint16_t buffer sizes, R16_FLOAT views |
| Output looked 30fps not 60fps | Present(0, ALLOW_TEARING) → Present(1, 0) — FLIP_DISCARD was silently discarding interpolated frames |
| MP4 stuttered every ~5 seconds | GOP I-frame decode spikes; fixed with decode-ahead ring buffer (feature/file-pacing) |
| Sleep imprecision causing bursts | Added timeBeginPeriod(1) for 1ms Windows timer resolution (feature/file-pacing) |

---

## Merging the Pacing Branch

Once tested on the capture card:

```powershell
git checkout master
git merge feature/file-pacing
git push origin master
```
