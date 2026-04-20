// FileSource.h
// Plays back an MP4/MKV/MOV file via Media Foundation Source Reader,
// delivering BGRA frames as D3D12 textures into a FrameQueue<CapturedFrame>.
//
// Use this to test the full Pipeline without a live capture card.
// Frames are CPU-decoded and uploaded to GPU via an upload heap.
// CapturedFrame::tex11 is always null — Pipeline handles this gracefully.
// The file loops indefinitely until Stop() is called.
//
// Note: output FPS will be limited by RIFE inference speed, not the file's
// native rate — same as in the live capture scenario.
#pragma once
#include "Common.h"
#include "D3DContext.h"
#include "FrameQueue.h"
#include "CaptureSource.h"  // CapturedFrame

class FileSource
{
public:
    FileSource(const std::wstring& filePath,
               FrameQueue<CapturedFrame>& queue,
               const D3DContext& ctx,
               bool loop = true);
    ~FileSource();

    void Start();
    void Stop();

    UINT   NativeWidth()  const { return videoWidth_;  }
    UINT   NativeHeight() const { return videoHeight_; }
    double NativeFPS()    const { return nativeFPS_;   }

private:
    void ThreadProc();
    void OpenFile();
    void AllocateUploadResources();

    FrameQueue<CapturedFrame>& queue_;
    const D3DContext&          ctx_;
    std::wstring               filePath_;

    ComPtr<IMFSourceReader>    reader_;
    UINT   videoWidth_  = 0;
    UINT   videoHeight_ = 0;
    double nativeFPS_   = 60.0;

    // Single pre-allocated CPU→GPU upload path.
    // gpuTex is reallocated per frame (fresh COPY_DEST state each time).
    // uploadHeap is reused — safe because we CPU-sync after every upload.
    ComPtr<ID3D12Resource>            uploadHeap_;
    UINT                              uploadRowPitch_ = 0;
    ComPtr<ID3D12CommandAllocator>    cmdAlloc_;
    ComPtr<ID3D12GraphicsCommandList> cmdList_;
    D3DContext::FenceSync             fence_;

    bool              loop_   = true;
    std::thread       thread_;
    std::atomic<bool> running_{ false };
};
