#include "AudioCaptureSource.h"
#include <cstdio>

std::vector<AudioCaptureDeviceInfo> AudioCaptureSource::EnumerateDevices()
{
    IMFAttributes* attrs = nullptr;
    HR_CHECK(MFCreateAttributes(&attrs, 1));
    ComPtr<IMFAttributes> attrGuard(attrs);

    HR_CHECK(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID));

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    HR_CHECK(MFEnumDeviceSources(attrs, &devices, &count));

    std::vector<AudioCaptureDeviceInfo> result;
    result.reserve(count);

    for (UINT32 i = 0; i < count; ++i)
    {
        AudioCaptureDeviceInfo info;

        WCHAR* name = nullptr;
        UINT32 nameLen = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen)))
        {
            info.friendlyName = name;
            CoTaskMemFree(name);
        }

        WCHAR* endpoint = nullptr;
        UINT32 endpointLen = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID,
            &endpoint, &endpointLen)))
        {
            info.endpointId = endpoint;
            CoTaskMemFree(endpoint);
        }

        result.push_back(std::move(info));
        devices[i]->Release();
    }

    CoTaskMemFree(devices);
    return result;
}

AudioCaptureSource::AudioCaptureSource(const std::wstring& preferredNameHint)
{
    flushEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!flushEvent_) throw std::runtime_error("CreateEvent failed");
    Setup(preferredNameHint);
}

AudioCaptureSource::~AudioCaptureSource()
{
    Stop();
    if (flushEvent_)
    {
        CloseHandle(flushEvent_);
        flushEvent_ = nullptr;
    }
}

void AudioCaptureSource::Setup(const std::wstring& preferredNameHint)
{
    auto devices = EnumerateDevices();
    if (devices.empty())
    {
        throw std::runtime_error("No audio capture devices found");
    }

    size_t chosen = 0;
    for (size_t i = 0; i < devices.size(); ++i)
    {
        if (!preferredNameHint.empty() &&
            devices[i].friendlyName.find(preferredNameHint) != std::wstring::npos)
        {
            chosen = i;
            break;
        }
        if (devices[i].friendlyName.find(L"Elgato") != std::wstring::npos)
        {
            chosen = i;
        }
    }

    printf("[audio] using capture endpoint: %ls\n", devices[chosen].friendlyName.c_str());
    fflush(stdout);

    ComPtr<IMFAttributes> attrs;
    HR_CHECK(MFCreateAttributes(&attrs, 3));
    HR_CHECK(attrs->SetUINT32(MF_LOW_LATENCY, TRUE));
    HR_CHECK(attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK,
                               static_cast<IMFSourceReaderCallback*>(this)));

    ComPtr<IMFAttributes> devAttrs;
    HR_CHECK(MFCreateAttributes(&devAttrs, 2));
    HR_CHECK(devAttrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                               MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID));
    HR_CHECK(devAttrs->SetString(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID,
        devices[chosen].endpointId.c_str()));

    ComPtr<IMFMediaSource> source;
    HR_CHECK(MFCreateDeviceSource(devAttrs.Get(), &source));
    HR_CHECK(MFCreateSourceReaderFromMediaSource(source.Get(), attrs.Get(), &reader_));

    ComPtr<IMFMediaType> outType;
    HR_CHECK(MFCreateMediaType(&outType));
    HR_CHECK(outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    HR_CHECK(outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
    HR_CHECK(reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                          nullptr, outType.Get()));

    ComPtr<IMFMediaType> curType;
    HR_CHECK(reader_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &curType));
    if (!player_.Open(curType.Get()))
        throw std::runtime_error("Audio player failed to open capture format");

    HR_CHECK(reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE));
    HR_CHECK(reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE));

    UINT32 ch = 0, hz = 0, bits = 0;
    curType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
    curType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &hz);
    curType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
    printf("[audio] enabled: %u Hz, %u-bit, %u ch\n", hz, bits, ch);
    fflush(stdout);
}

void AudioCaptureSource::Start()
{
    stop_ = false;
    RequestNextSample();
}

void AudioCaptureSource::Stop()
{
    stop_ = true;
    if (reader_)
    {
        reader_->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS);
        WaitForSingleObject(flushEvent_, 1000);
    }
    player_.Stop();
}

void AudioCaptureSource::RequestNextSample()
{
    if (!stop_ && reader_)
    {
        reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                            0, nullptr, nullptr, nullptr, nullptr);
    }
}

HRESULT AudioCaptureSource::OnReadSample(HRESULT hr, DWORD /*streamIndex*/, DWORD streamFlags,
                                         LONGLONG /*timestamp*/, IMFSample* pSample)
{
    if (stop_) return S_OK;

    if (FAILED(hr))
    {
        printf("[audio] OnReadSample error 0x%08X\n", (unsigned)hr);
        fflush(stdout);
        return hr;
    }

    if ((streamFlags & MF_SOURCE_READERF_STREAMTICK) == 0 && pSample)
        player_.PlaySample(pSample);

    RequestNextSample();
    return S_OK;
}

HRESULT AudioCaptureSource::OnFlush(DWORD /*streamIndex*/)
{
    SetEvent(flushEvent_);
    return S_OK;
}

HRESULT AudioCaptureSource::OnEvent(DWORD /*streamIndex*/, IMFMediaEvent* /*ev*/)
{
    return S_OK;
}

HRESULT AudioCaptureSource::QueryInterface(REFIID riid, void** ppv)
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

ULONG AudioCaptureSource::AddRef() { return ++refCount_; }
ULONG AudioCaptureSource::Release() { return --refCount_; }
