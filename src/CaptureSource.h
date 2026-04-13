// CaptureSource.h
// Enumerates and opens a live HDMI capture device via Media Foundation
// Source Reader (IMFSourceReader).  Delivers BGRA frames as D3D12 textures
// backed by a shared D3D11On12 device, compatible with TextureConverter.
//
// Architecture:
//   - Capture device opened with MF_LOW_LATENCY + hardware D3D11 path.
//   - Async callback (IMFSourceReaderCallback) receives each sample on an
//     MF thread-pool thread.
//   - Frame pushed into caller-supplied FrameQueue<CapturedFrame>.
//   - Stale frames (queue full) are silently dropped to maintain freshness.
//
// Usage:
//   1. CaptureSource::EnumerateDevices() → pick index.
//   2. Construct CaptureSource with device index, queue, and D3DContext.
//   3. Call Start(); frames begin arriving in the queue.
//   4. Call Stop() to drain and shut down.
#pragma once
#include "Common.h"
#include "D3DContext.h"
#include "FrameQueue.h"
#include "AudioPlayer.h"

// One live-captured, GPU-resident video frame.
struct CapturedFrame
{
    ComPtr<ID3D12Resource>  texBGRA;     // DXGI_FORMAT_B8G8R8A8_UNORM D3D12 texture
    ComPtr<ID3D11Texture2D> tex11;       // D3D11 side; must call ReturnUnderlyingResource
    UINT   width  = 0;
    UINT   height = 0;
    LONGLONG captureTime = 0;            // QueryPerformanceCounter at push time
};

struct CaptureDeviceInfo
{
    std::wstring friendlyName;
    std::wstring symbolicLink;
};

class CaptureSource : public IMFSourceReaderCallback
{
public:
    // Returns all available video capture devices.
    static std::vector<CaptureDeviceInfo> EnumerateDevices();

    // deviceIndex: 0-based index into EnumerateDevices() result list.
    CaptureSource(UINT deviceIndex,
                  FrameQueue<CapturedFrame>& queue,
                  const D3DContext& ctx);
    ~CaptureSource();

    // Start asynchronous capture.
    void Start();

    // Stop capture and wait for the MF flush to complete.
    void Stop();

    UINT NativeWidth()  const { return videoWidth_;  }
    UINT NativeHeight() const { return videoHeight_; }
    double NativeFPS()  const { return nativeFPS_;   }

    // ── IMFSourceReaderCallback ─────────────────────────────────────────────
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)()                         override;
    STDMETHOD_(ULONG, Release)()                        override;

    STDMETHOD(OnReadSample)(HRESULT hr, DWORD streamIndex, DWORD streamFlags,
                            LONGLONG timestamp, IMFSample* pSample) override;
    STDMETHOD(OnFlush)(DWORD streamIndex)               override;
    STDMETHOD(OnEvent)(DWORD streamIndex, IMFMediaEvent*) override;

private:
    void SetupSourceReader(const std::wstring& symbolicLink);
    void RequestNextSample(DWORD streamIndex);

    FrameQueue<CapturedFrame>& queue_;
    const D3DContext&          ctx_;

    ComPtr<IMFSourceReader>         reader_;
    ComPtr<IMFDXGIDeviceManager>    dxgiMgr_;
    UINT                            resetToken_ = 0;

    UINT     videoWidth_  = 0;
    UINT     videoHeight_ = 0;
    double   nativeFPS_   = 60.0;
    bool     hasAudio_    = false;

    AudioPlayer audioPlayer_;

    std::atomic<bool> stop_{ false };

    // COM ref-count (CaptureSource is stack/unique_ptr owned; this is for MF).
    std::atomic<ULONG> refCount_{ 1 };

    // Event signalled when MF flush completes (used in Stop()).
    HANDLE flushEvent_ = nullptr;

    // ── D3D12 staging copy infrastructure ──────────────────────────────────
    // MF reuses a small texture pool; holding UnwrapUnderlyingResource until
    // GPU work finishes (> 16 ms at 60 fps) causes E_INVALIDARG when MF tries
    // to deliver the next frame using the same pooled texture.
    // Fix: copy the MF texture into our own staging D3D12 resource immediately,
    // call ReturnUnderlyingResource right in the callback, and deliver the
    // staging copy downstream.  Staging textures are round-robined; 6 slots
    // cover captureQueue_ depth-3 back-pressure + 1 in RifeThread + 1 in flight.
    static constexpr int kStagingCount = 6;
    ComPtr<ID3D12Resource>            stagingTex_[kStagingCount];
    int                               stagingIdx_     = 0;
    ComPtr<ID3D12CommandAllocator>    copyAlloc_;
    ComPtr<ID3D12GraphicsCommandList> copyCmdList_;
    ComPtr<ID3D12Fence>               copyFence_;
    UINT64                            copyFenceVal_   = 0;
    HANDLE                            copyFenceEvent_ = nullptr;
};
