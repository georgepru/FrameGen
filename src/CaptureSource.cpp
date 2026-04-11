// CaptureSource.cpp
#include "CaptureSource.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdexcept>
#include <cassert>
#include <cstdio>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "evr.lib")
#pragma comment(lib, "ole32.lib")

// ---------------------------------------------------------------------------
// Static: enumerate all video capture devices via MFEnumDeviceSources.
// ---------------------------------------------------------------------------
std::vector<CaptureDeviceInfo> CaptureSource::EnumerateDevices()
{
    // MFStartup must have been called by the caller (done in Pipeline ctor).
    IMFAttributes* attrs = nullptr;
    HR_CHECK(MFCreateAttributes(&attrs, 1));
    ComPtr<IMFAttributes> attrGuard(attrs);

    HR_CHECK(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    HR_CHECK(MFEnumDeviceSources(attrs, &devices, &count));

    std::vector<CaptureDeviceInfo> result;
    result.reserve(count);

    for (UINT32 i = 0; i < count; ++i)
    {
        CaptureDeviceInfo info;

        WCHAR* name = nullptr;
        UINT32  nameLen = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen)))
        {
            info.friendlyName = name;
            CoTaskMemFree(name);
        }

        WCHAR* sym = nullptr;
        UINT32  symLen = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                &sym, &symLen)))
        {
            info.symbolicLink = sym;
            CoTaskMemFree(sym);
        }

        result.push_back(std::move(info));
        devices[i]->Release();
    }

    CoTaskMemFree(devices);
    return result;
}

// ---------------------------------------------------------------------------
CaptureSource::CaptureSource(UINT deviceIndex,
                             FrameQueue<CapturedFrame>& queue,
                             const D3DContext& ctx)
    : queue_(queue), ctx_(ctx)
{
    flushEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!flushEvent_) throw std::runtime_error("CreateEvent failed");

    auto devices = EnumerateDevices();
    if (deviceIndex >= devices.size())
        throw std::runtime_error("Capture device index out of range");

    printf("[capture] opening: %ls\n", devices[deviceIndex].friendlyName.c_str());
    fflush(stdout);

    SetupSourceReader(devices[deviceIndex].symbolicLink);

    // Allocate staging D3D12 textures + copy command infrastructure.
    // Resolution is known now (SetupSourceReader populated videoWidth_/Height_).
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width              = videoWidth_;
        rd.Height             = videoHeight_;
        rd.DepthOrArraySize   = 1;
        rd.MipLevels          = 1;
        rd.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
        rd.SampleDesc.Count   = 1;
        rd.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags              = D3D12_RESOURCE_FLAG_NONE;

        for (int i = 0; i < kStagingCount; ++i)
            HR_CHECK(ctx_.device12->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(&stagingTex_[i])));

        HR_CHECK(ctx_.device12->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copyAlloc_)));
        HR_CHECK(ctx_.device12->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, copyAlloc_.Get(), nullptr,
            IID_PPV_ARGS(&copyCmdList_)));
        copyCmdList_->Close();

        HR_CHECK(ctx_.device12->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence_)));
        copyFenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!copyFenceEvent_)
            throw std::runtime_error("CreateEvent failed for copy fence");
    }
}

// ---------------------------------------------------------------------------
CaptureSource::~CaptureSource()
{
    Stop();

    if (copyFenceEvent_)
    {
        CloseHandle(copyFenceEvent_);
        copyFenceEvent_ = nullptr;
    }

    if (flushEvent_)
    {
        CloseHandle(flushEvent_);
        flushEvent_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
void CaptureSource::SetupSourceReader(const std::wstring& symbolicLink)
{
    // ── DXGI device manager (shares our D3D11On12 device with MF) ──────────
    HR_CHECK(MFCreateDXGIDeviceManager(&resetToken_, &dxgiMgr_));
    HR_CHECK(dxgiMgr_->ResetDevice(ctx_.device11.Get(), resetToken_));

    // ── Source-reader attributes ────────────────────────────────────────────
    ComPtr<IMFAttributes> srAttrs;
    HR_CHECK(MFCreateAttributes(&srAttrs, 8));

    // D3D11 hardware path for zero-copy GPU frames
    HR_CHECK(srAttrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dxgiMgr_.Get()));
    HR_CHECK(srAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE));
    HR_CHECK(srAttrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
    // Async callback (all frames delivered on MF thread pool)
    HR_CHECK(srAttrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK,
                                  static_cast<IMFSourceReaderCallback*>(this)));
    // Low-latency flag: reduce internal MF decoder buffering
    HR_CHECK(srAttrs->SetUINT32(MF_LOW_LATENCY, TRUE));

    // ── Open device by symbolic link ─────────────────────────────────────────
    // Build a device-source attribute set pointing to the chosen capture card.
    ComPtr<IMFAttributes> devAttrs;
    HR_CHECK(MFCreateAttributes(&devAttrs, 2));
    HR_CHECK(devAttrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                               MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
    HR_CHECK(devAttrs->SetString(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
        symbolicLink.c_str()));

    ComPtr<IMFMediaSource> source;
    HR_CHECK(MFCreateDeviceSource(devAttrs.Get(), &source));

    HR_CHECK(MFCreateSourceReaderFromMediaSource(source.Get(), srAttrs.Get(), &reader_));

    // ── Query native video format ────────────────────────────────────────────
    // Pick the first native type to get resolution + frame rate.
    ComPtr<IMFMediaType> nativeType;
    HRESULT hrNative = reader_->GetNativeMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nativeType);

    if (SUCCEEDED(hrNative))
    {
        UINT64 packed = 0;
        if (SUCCEEDED(nativeType->GetUINT64(MF_MT_FRAME_SIZE, &packed)))
        {
            videoWidth_  = (UINT)(packed >> 32);
            videoHeight_ = (UINT)(packed & 0xFFFFFFFFu);
        }

        UINT64 rateNum = 0;
        if (SUCCEEDED(nativeType->GetUINT64(MF_MT_FRAME_RATE, &rateNum)))
        {
            UINT32 num = (UINT32)(rateNum >> 32);
            UINT32 den = (UINT32)(rateNum & 0xFFFFFFFFu);
            if (den > 0) nativeFPS_ = static_cast<double>(num) / den;
        }
    }

    printf("[capture] native: %ux%u @ %.2f fps\n",
           videoWidth_, videoHeight_, nativeFPS_);
    fflush(stdout);

    // ── Request BGRA output ─────────────────────────────────────────────────
    // MF's GPU video processor converts whatever the card delivers (NV12/YUY2/etc)
    // to BGRA on the GPU, giving us a standard D3D11 render-target texture.
    ComPtr<IMFMediaType> outType;
    HR_CHECK(MFCreateMediaType(&outType));
    HR_CHECK(outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    HR_CHECK(outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32));
    HR_CHECK(reader_->SetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outType.Get()));

    // Re-read dimensions from the output type in case ARGB32 changes them.
    {
        ComPtr<IMFMediaType> curType;
        if (SUCCEEDED(reader_->GetCurrentMediaType(
                (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &curType)))
        {
            UINT64 packed = 0;
            if (SUCCEEDED(curType->GetUINT64(MF_MT_FRAME_SIZE, &packed)))
            {
                videoWidth_  = (UINT)(packed >> 32);
                videoHeight_ = (UINT)(packed & 0xFFFFFFFFu);
            }
        }
    }

    // Disable audio streams (we only care about video for this MVP).
    HR_CHECK(reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE));
    HR_CHECK(reader_->SetStreamSelection(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE));

    printf("[capture] output type set: %ux%u BGRA\n", videoWidth_, videoHeight_);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
void CaptureSource::Start()
{
    RequestNextSample();
}

void CaptureSource::Stop()
{
    stop_ = true;
    queue_.Interrupt();
    if (reader_)
    {
        reader_->Flush((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        // OnFlush will signal flushEvent_.
        WaitForSingleObject(flushEvent_, 3000);
    }
}

void CaptureSource::RequestNextSample()
{
    if (!stop_ && reader_)
        reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                            0, nullptr, nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// IMFSourceReaderCallback::OnReadSample
// Called on an MF thread-pool thread for every decoded/processed sample.
// ---------------------------------------------------------------------------
HRESULT CaptureSource::OnReadSample(HRESULT hr, DWORD /*streamIndex*/,
                                     DWORD streamFlags, LONGLONG /*timestamp*/,
                                     IMFSample* pSample)
{
    // Catch all C++ exceptions — this runs on an MF thread-pool thread.
    // An unhandled exception here calls std::terminate() with no log output.
    try {

    if (stop_) return S_OK;

    if (FAILED(hr))
    {
        printf("[capture] OnReadSample error 0x%08X\n", (unsigned)hr);
        fflush(stdout);
        queue_.Interrupt();
        return hr;
    }

    // Stream errors / device lost
    if (streamFlags & MF_SOURCE_READERF_ERROR)
    {
        printf("[capture] stream error flag\n"); fflush(stdout);
        queue_.Interrupt();
        return S_OK;
    }

    if (!pSample)
    {
        // No sample yet (e.g. device not sending) — request again.
        RequestNextSample();
        return S_OK;
    }

    // ── Extract the D3D11 texture ───────────────────────────────────────────
    ComPtr<IMFMediaBuffer> buf;
    HRESULT bufHr = pSample->GetBufferByIndex(0, &buf);
    if (FAILED(bufHr))
    {
        printf("[capture] GetBufferByIndex failed 0x%08X\n", (unsigned)bufHr);
        fflush(stdout);
        RequestNextSample();
        return S_OK;
    }

    ComPtr<IMFDXGIBuffer> dxgiBuf;
    if (FAILED(buf->QueryInterface(IID_PPV_ARGS(&dxgiBuf))))
    {
        printf("[capture] no DXGI buffer on sample, skipping\n"); fflush(stdout);
        RequestNextSample();
        return S_OK;
    }

    ComPtr<ID3D11Texture2D> tex11;
    UINT subresource = 0;
    if (FAILED(dxgiBuf->GetResource(IID_PPV_ARGS(&tex11))))
    {
        RequestNextSample();
        return S_OK;
    }
    dxgiBuf->GetSubresourceIndex(&subresource);

    // ── Unwrap MF texture, copy to our staging resource, return MF tex now ───
    // At 60 fps, MF's 2–3 pooled textures are recycled every ~16 ms.  If the
    // RifeThread hasn't called ReturnUnderlyingResource on the previous frame's
    // MF texture by the time the next callback fires, UnwrapUnderlyingResource
    // returns E_INVALIDARG (resource still "owned" by D3D12).
    // Fix: immediately after getting the D3D12 handle, copy to our own staging
    // texture, synchronously wait for the copy, then return the MF texture so
    // its pool slot is free before we even push to the capture queue.
    ComPtr<ID3D12Resource> mfRes12;
    HRESULT wrapHr = ctx_.on12->UnwrapUnderlyingResource(
        tex11.Get(), ctx_.cmdQueue12.Get(), IID_PPV_ARGS(&mfRes12));

    if (FAILED(wrapHr))
    {
        printf("[capture] UnwrapUnderlyingResource failed 0x%08X, skipping\n",
               (unsigned)wrapHr);
        fflush(stdout);
        RequestNextSample();
        return S_OK;
    }

    // Round-robin staging slot selection.
    const int usedSlot  = stagingIdx_;
    stagingIdx_         = (stagingIdx_ + 1) % kStagingCount;
    ID3D12Resource* stagingDst = stagingTex_[usedSlot].Get();

    // Record copy: mfRes12 → stagingDst (both textures start in COMMON).
    HR_CHECK(copyAlloc_->Reset());
    HR_CHECK(copyCmdList_->Reset(copyAlloc_.Get(), nullptr));

    D3D12_RESOURCE_BARRIER bars[2] = {};
    // mfRes12: COMMON → COPY_SOURCE
    bars[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[0].Transition.pResource   = mfRes12.Get();
    bars[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    bars[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bars[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    // stagingDst: COMMON → COPY_DEST
    bars[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[1].Transition.pResource   = stagingDst;
    bars[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    bars[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    bars[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    copyCmdList_->ResourceBarrier(2, bars);

    copyCmdList_->CopyResource(stagingDst, mfRes12.Get());

    // Transition both back to COMMON.
    // stagingDst must be COMMON when TextureConverter::BGRAtoNCHW reads it.
    bars[0].Transition.pResource   = mfRes12.Get();
    bars[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bars[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    bars[1].Transition.pResource   = stagingDst;
    bars[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bars[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    copyCmdList_->ResourceBarrier(2, bars);

    HR_CHECK(copyCmdList_->Close());
    ID3D12CommandList* lists[] = { copyCmdList_.Get() };
    ctx_.cmdQueue12->ExecuteCommandLists(1, lists);

    // CPU-wait for copy completion.
    // ReturnUnderlyingResource requires all D3D12 GPU work on mfRes12 to finish.
    UINT64 fenceVal = ++copyFenceVal_;
    HR_CHECK(ctx_.cmdQueue12->Signal(copyFence_.Get(), fenceVal));
    if (copyFence_->GetCompletedValue() < fenceVal)
    {
        copyFence_->SetEventOnCompletion(fenceVal, copyFenceEvent_);
        WaitForSingleObject(copyFenceEvent_, INFINITE);
    }

    // Return MF texture immediately — pool slot is free before the next callback.
    ctx_.on12->ReturnUnderlyingResource(tex11.Get(), 0, nullptr, nullptr);

    // ── Build CapturedFrame and push ────────────────────────────────────────
    CapturedFrame frame;
    frame.texBGRA     = stagingTex_[usedSlot];  // our D3D12 copy (in COMMON state)
    frame.tex11       = nullptr;                 // MF texture already returned above
    frame.width       = videoWidth_;
    frame.height      = videoHeight_;
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    frame.captureTime = qpc.QuadPart;

    // Back-pressure: blocks if queue is full (depth 3), false only on interrupt.
    if (!queue_.Push(std::move(frame)))
    {
        // Shutdown path — staging slot reclaimed on next round-robin without
        // any D3D11 cleanup (MF texture was already returned above).
    }

    RequestNextSample();
    return S_OK;

    } // try
    catch (const std::exception& e)
    {
        printf("[capture] FATAL exception in OnReadSample: %s\n", e.what()); fflush(stdout);
        queue_.Interrupt();
        return E_FAIL;
    }
    catch (...)
    {
        printf("[capture] FATAL unknown exception in OnReadSample\n"); fflush(stdout);
        queue_.Interrupt();
        return E_FAIL;
    }
}
// ---------------------------------------------------------------------------
HRESULT CaptureSource::OnFlush(DWORD /*streamIndex*/)
{
    SetEvent(flushEvent_);
    return S_OK;
}

HRESULT CaptureSource::OnEvent(DWORD /*streamIndex*/, IMFMediaEvent* /*pEvent*/)
{
    return S_OK;
}

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------
HRESULT CaptureSource::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IMFSourceReaderCallback))
    {
        *ppv = static_cast<IMFSourceReaderCallback*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG CaptureSource::AddRef()  { return ++refCount_; }
ULONG CaptureSource::Release() { return --refCount_; }
