// FileSource.cpp
#include "FileSource.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <cstdio>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "propsys.lib")

// ---------------------------------------------------------------------------
FileSource::FileSource(const std::wstring& filePath,
                       FrameQueue<CapturedFrame>& queue,
                       const D3DContext& ctx)
    : filePath_(filePath), queue_(queue), ctx_(ctx)
{
    OpenFile();
    AllocateUploadResources();
}

// ---------------------------------------------------------------------------
FileSource::~FileSource()
{
    Stop();
}

// ---------------------------------------------------------------------------
void FileSource::OpenFile()
{
    // No D3D device manager — CPU decode, simpler for testing.
    ComPtr<IMFAttributes> attr;
    HR_CHECK(MFCreateAttributes(&attr, 4));
    HR_CHECK(attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE));

    HR_CHECK(MFCreateSourceReaderFromURL(filePath_.c_str(), attr.Get(), &reader_));

    // Disable all streams, then enable first video stream only.
    HR_CHECK(reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE));
    HR_CHECK(reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE));

    // Request BGRA output (MFVideoFormat_ARGB32 = B8G8R8A8 in memory).
    ComPtr<IMFMediaType> mt;
    HR_CHECK(MFCreateMediaType(&mt));
    HR_CHECK(mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    HR_CHECK(mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32));
    HR_CHECK(reader_->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt.Get()));

    // Query resolved output type for dimensions and frame rate.
    ComPtr<IMFMediaType> actual;
    HR_CHECK(reader_->GetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual));

    UINT32 w = 0, h = 0;
    MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &w, &h);
    videoWidth_  = w;
    videoHeight_ = h;

    UINT32 fpsNum = 0, fpsDen = 1;
    MFGetAttributeRatio(actual.Get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
    if (fpsNum > 0 && fpsDen > 0)
        nativeFPS_ = static_cast<double>(fpsNum) / fpsDen;

    if (videoWidth_ == 0 || videoHeight_ == 0)
        throw std::runtime_error("FileSource: could not determine video dimensions");

    printf("[filesource] opened %ls: %ux%u @ %.2f fps\n",
           filePath_.c_str(), videoWidth_, videoHeight_, nativeFPS_);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
void FileSource::AllocateUploadResources()
{
    // Compute the aligned row pitch D3D12 needs for a BGRA texture upload.
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = videoWidth_;
    texDesc.Height           = videoHeight_;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc       = { 1, 0 };

    UINT64 uploadSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    ctx_.device12->GetCopyableFootprints(
        &texDesc, 0, 1, 0, &footprint, nullptr, nullptr, &uploadSize);
    uploadRowPitch_ = footprint.Footprint.RowPitch;

    // Single upload heap — reused every frame after CPU sync.
    uploadHeap_ = ctx_.CreateBuffer(
        uploadSize,
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);

    // Command infra for CopyTextureRegion.
    HR_CHECK(ctx_.device12->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc_)));
    HR_CHECK(ctx_.device12->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        cmdAlloc_.Get(), nullptr, IID_PPV_ARGS(&cmdList_)));
    HR_CHECK(cmdList_->Close());

    fence_ = ctx_.CreateFenceSync();
}

// ---------------------------------------------------------------------------
void FileSource::Start()
{
    running_ = true;
    thread_  = std::thread([this]{ ThreadProc(); });
}

// ---------------------------------------------------------------------------
void FileSource::Stop()
{
    running_ = false;
    queue_.Interrupt();
    if (thread_.joinable()) thread_.join();
    reader_ = nullptr;
}

// ---------------------------------------------------------------------------
void FileSource::ThreadProc()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    printf("[filesource] thread started\n"); fflush(stdout);

    try
    {
    while (running_)
    {
        DWORD    flags     = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;

        HRESULT hr = reader_->ReadSample(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            0, nullptr, &flags, &timestamp, &sample);

        if (FAILED(hr) || !running_) break;

        // End of stream → loop back to start.
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
        {
            PROPVARIANT var;
            PropVariantInit(&var);
            var.vt            = VT_I8;
            var.hVal.QuadPart = 0;  // seek to position 0 (100-ns units)
            reader_->SetCurrentPosition(GUID_NULL, var);
            PropVariantClear(&var);
            printf("[filesource] looping\n");
            fflush(stdout);
            continue;
        }

        if (!sample) continue;

        // Get contiguous BGRA bytes.
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;

        BYTE* data   = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(buf->Lock(&data, &maxLen, &curLen)))  continue;

        const UINT srcStride = videoWidth_ * 4;  // 4 bytes per BGRA pixel

        // Map upload heap and copy row-by-row (MF stride == srcStride for
        // contiguous buffer, but D3D12 row pitch may be larger).
        BYTE* mapped = nullptr;
        D3D12_RANGE readRange{ 0, 0 };
        HR_CHECK(uploadHeap_->Map(0, &readRange, reinterpret_cast<void**>(&mapped)));
        for (UINT y = 0; y < videoHeight_; ++y)
        {
            memcpy(mapped + y * uploadRowPitch_,
                   data   + y * srcStride,
                   srcStride);
        }
        uploadHeap_->Unmap(0, nullptr);
        buf->Unlock();

        // Allocate a fresh GPU texture (always starts in COPY_DEST).
        // Reallocating per frame avoids re-transitioning a shared texture.
        ComPtr<ID3D12Resource> gpuTex = ctx_.CreateTexture2D(
            videoWidth_, videoHeight_,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST);

        // Record: CopyTextureRegion + barrier COPY_DEST → COMMON.
        HR_CHECK(cmdAlloc_->Reset());
        HR_CHECK(cmdList_->Reset(cmdAlloc_.Get(), nullptr));

        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource                          = uploadHeap_.Get();
        srcLoc.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_B8G8R8A8_UNORM;
        srcLoc.PlacedFootprint.Footprint.Width    = videoWidth_;
        srcLoc.PlacedFootprint.Footprint.Height   = videoHeight_;
        srcLoc.PlacedFootprint.Footprint.Depth    = 1;
        srcLoc.PlacedFootprint.Footprint.RowPitch = uploadRowPitch_;

        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource        = gpuTex.Get();
        dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;

        cmdList_->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = gpuTex.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList_->ResourceBarrier(1, &barrier);

        HR_CHECK(cmdList_->Close());

        ID3D12CommandList* lists[] = { cmdList_.Get() };
        ctx_.cmdQueue12->ExecuteCommandLists(1, lists);

        // CPU-wait for GPU copy — ensures upload heap is safe to reuse and the
        // texture is fully resident before pushing to queue.
        fence_.Signal(ctx_.cmdQueue12.Get());
        fence_.Wait();

        // Push frame. Push blocks if queue is full (back-pressure).
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);

        CapturedFrame frame;
        frame.texBGRA     = gpuTex;   // ComPtr keeps texture alive through pipeline
        frame.tex11       = nullptr;  // no D3D11On12 — Pipeline checks for null
        frame.width       = videoWidth_;
        frame.height      = videoHeight_;
        frame.captureTime = qpc.QuadPart;

        queue_.Push(frame);
    }
    } // while
    catch (const std::exception& e)
    {
        printf("[filesource] EXCEPTION: %s\n", e.what()); fflush(stdout);
    }
    catch (...)
    {
        printf("[filesource] UNKNOWN EXCEPTION\n"); fflush(stdout);
    }

    printf("[filesource] thread exiting\n"); fflush(stdout);
    CoUninitialize();
}
