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
}

// ---------------------------------------------------------------------------
CaptureSource::~CaptureSource()
{
    Stop();

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

    // ── Unwrap the D3D11On12 texture to get its underlying D3D12 resource ───
    // MF created this texture on our D3D11On12 device, so it has a D3D12
    // resource underneath.  UnwrapUnderlyingResource flushes D3D11 work and
    // hands the D3D12 resource to the specified queue for D3D12 use.
    // Caller (Pipeline) must call ReturnUnderlyingResource when GPU work done.
    ComPtr<ID3D12Resource> res12;
    HRESULT wrapHr = ctx_.on12->UnwrapUnderlyingResource(
        tex11.Get(), ctx_.cmdQueue12.Get(), IID_PPV_ARGS(&res12));

    if (FAILED(wrapHr))
    {
        // MF gave us a texture we can't unwrap (e.g. from a different device).
        // Skip frame — next callback will try again.
        printf("[capture] UnwrapUnderlyingResource failed 0x%08X, skipping\n",
               (unsigned)wrapHr);
        fflush(stdout);
        RequestNextSample();
        return S_OK;
    }

    // ── Build CapturedFrame and push ────────────────────────────────────────
    CapturedFrame frame;
    frame.texBGRA     = res12;
    frame.tex11       = tex11;
    frame.width       = videoWidth_;
    frame.height      = videoHeight_;
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    frame.captureTime = qpc.QuadPart;

    // Push: if full, drop this frame and stay fresh.
    // Non-blocking try: if the queue is full we just skip this frame.
    if (!queue_.Push(std::move(frame)))
    {
        // Queue interrupted or rejected — return D3D11 resource immediately.
        ctx_.on12->ReturnUnderlyingResource(tex11.Get(), 0, nullptr, nullptr);
    }

    RequestNextSample();
    return S_OK;
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
