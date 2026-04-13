// Pipeline.h
// Owns and coordinates three worker threads:
//   1. CaptureThread  – drives CaptureSource, pushes CapturedFrame
//   2. RifeThread     – converts BGRA→NCHW, runs inference, pushes PresentFrame
//   3. PresentThread  – waits for PresentFrame, calls SwapPresenter::Present
//
// All GPU work stays on the GPU; no CPU readback in the hot path.
#pragma once
#include "Common.h"
#include "D3DContext.h"
#include "FrameQueue.h"
#include "CaptureSource.h"
#include "AudioCaptureSource.h"
#include "FileSource.h"
#include "NearDuplicateDetector.h"
#include "TextureConverter.h"
#include "RifeInference.h"
#include "SwapPresenter.h"
#include "Telemetry.h"
#include "Overlay.h"

struct PresentFrame
{
    ID3D12Resource* nchwBuf  = nullptr;   // weak ref into RifeInference buffers
    ID3D12Resource* bgraRef  = nullptr;   // optional: original BGRA copy for compare mode right half
    UINT            vidW     = 0;
    UINT            vidH     = 0;
    UINT            paddedW  = 0;
    UINT            paddedH  = 0;
    bool            needsDwmFlushAfter = false; // true for interpolated frame: wait for DWM refresh before next present
};

class Pipeline
{
public:
    struct Config
    {
        UINT         deviceIndex = 0;     // capture device index (see CaptureSource::EnumerateDevices)
        std::wstring captureDeviceName;    // friendly name of selected video device (used to match audio endpoint)
        std::wstring filePath;             // if non-empty, use FileSource (MP4/MKV) instead of capture card
        std::wstring onnxPath;
        std::wstring logPath;             // telemetry CSV (empty = no log)
        HWND         hwnd       = nullptr;
        bool         debugD3D   = false;
        bool         compareMode    = false; // --compare: show interpolated left, original right
        bool         noOverlay      = true;  // default safe mode: overlay disabled unless --overlay is passed
        bool         noAudio        = false; // --no-audio: disable separate audio endpoint capture/playback
        bool         gpuDedupe      = false; // --gpu-dedupe: skip near-identical captured frames
        UINT         dedupeThreshold = 12;   // max changed sampled pixels to treat as duplicate
        bool         halfRateInput  = false; // --half-rate: consume every other input frame (30fps game in 60fps container)
        bool         fourXMode      = false; // --4x: 3-pass recursive interp 30→120fps (requires 120Hz display)
        UINT         screenW    = 0;      // full screen dimensions for compare mode swapchain
        UINT         screenH    = 0;
    };

    explicit Pipeline(const Config& cfg);
    ~Pipeline();

    void Start();
    void Stop();

    void ToggleInterpolation() { interpolation_.store(!interpolation_.load()); }
    void SetInterpolation(bool en) { interpolation_.store(en); }
    bool IsInterpolating()   const { return interpolation_.load(); }

    void ToggleOverlay() { showOverlay_.store(!showOverlay_.load()); }
    bool IsOverlayVisible() const { return showOverlay_.load(); }

    bool IsRunning()    const { return running_.load(); }
    bool ThreadFailed() const { return threadFailed_.load(); }
    std::string ThreadError() const { return threadError_; }

    const Telemetry& GetTelemetry() const { return *telemetry_; }

private:
    void CaptureThread();
    void RifeThread();
    void PresentThread();

    Config cfg_;
    HWND   hwnd_ = nullptr;

    std::unique_ptr<D3DContext>       ctx_;
    std::unique_ptr<CaptureSource>    capture_;
    std::unique_ptr<AudioCaptureSource> audioCapture_;
    std::unique_ptr<NearDuplicateDetector> dedupeDetector_;
    std::unique_ptr<FileSource>       fileSource_;
    std::unique_ptr<TextureConverter> converter_;
    std::unique_ptr<RifeInference>    rife_;
    std::unique_ptr<SwapPresenter>    presenter_;
    std::unique_ptr<Overlay>          overlay_;
    std::unique_ptr<Telemetry>        telemetry_;

    FrameQueue<CapturedFrame> captureQueue_{ 3 };  // shallow — drop old frames
    FrameQueue<PresentFrame>  presentQueue_{ 8 };  // 4x mode pushes 4 frames per pair

    ComPtr<ID3D12Resource>    refTex_;            // D3D12 BGRA copy used as compare-mode right panel
    ComPtr<ID3D12Resource>    scratch0_;           // 4x mode: holds M = interp(A,B)
    ComPtr<ID3D12Resource>    scratch1_;           // 4x mode: holds B while InBuf1 is overwritten
    ComPtr<ID3D12Resource>    scratch2_;           // 4x mode: stable copy of Q1 so OutBuf is free for Pass 3
    ComPtr<ID3D12Resource>    scratch3_;           // 4x mode: stable copy of Q3 so OutBuf is free for next pair's Pass 1

    std::thread captureThread_;
    std::thread rifeThread_;
    std::thread presentThread_;

    std::atomic<bool>   running_{ false };
    std::atomic<bool>   interpolation_{ true };
    std::atomic<bool>   showOverlay_{ true };
    std::atomic<bool>   threadFailed_{ false };
    std::string         threadError_;
};
