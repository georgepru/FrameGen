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
#include "FileSource.h"
#include "TextureConverter.h"
#include "RifeInference.h"
#include "SwapPresenter.h"
#include "Telemetry.h"
#include "Overlay.h"

struct PresentFrame
{
    ID3D12Resource* nchwBuf  = nullptr;   // weak ref into RifeInference buffers
    UINT            vidW     = 0;
    UINT            vidH     = 0;
    UINT            paddedW  = 0;
    UINT            paddedH  = 0;
};

class Pipeline
{
public:
    struct Config
    {
        UINT         deviceIndex = 0;     // capture device index (see CaptureSource::EnumerateDevices)
        std::wstring filePath;             // if non-empty, use FileSource (MP4/MKV) instead of capture card
        std::wstring onnxPath;
        std::wstring logPath;             // telemetry CSV (empty = no log)
        HWND         hwnd       = nullptr;
        bool         debugD3D   = false;
    };

    explicit Pipeline(const Config& cfg);
    ~Pipeline();

    void Start();
    void Stop();

    void ToggleInterpolation() { interpolation_.store(!interpolation_.load()); }
    void SetInterpolation(bool en) { interpolation_.store(en); }
    bool IsInterpolating()   const { return interpolation_.load(); }

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
    std::unique_ptr<FileSource>       fileSource_;
    std::unique_ptr<TextureConverter> converter_;
    std::unique_ptr<RifeInference>    rife_;
    std::unique_ptr<SwapPresenter>    presenter_;
    std::unique_ptr<Overlay>          overlay_;
    std::unique_ptr<Telemetry>        telemetry_;

    FrameQueue<CapturedFrame> captureQueue_{ 3 };  // shallow — drop old frames
    FrameQueue<PresentFrame>  presentQueue_{ 2 };

    std::thread captureThread_;
    std::thread rifeThread_;
    std::thread presentThread_;

    std::atomic<bool>   running_{ false };
    std::atomic<bool>   interpolation_{ true };
    std::atomic<bool>   threadFailed_{ false };
    std::string         threadError_;
};
