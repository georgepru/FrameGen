#pragma once
#include "Common.h"
#include "AudioPlayer.h"

struct AudioCaptureDeviceInfo
{
    std::wstring friendlyName;
    std::wstring endpointId;
};

class AudioCaptureSource : public IMFSourceReaderCallback
{
public:
    static std::vector<AudioCaptureDeviceInfo> EnumerateDevices();

    explicit AudioCaptureSource(const std::wstring& preferredNameHint);
    ~AudioCaptureSource();

    void Start();
    void Stop();

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    STDMETHOD(OnReadSample)(HRESULT hr, DWORD streamIndex, DWORD streamFlags,
                            LONGLONG timestamp, IMFSample* pSample) override;
    STDMETHOD(OnFlush)(DWORD streamIndex) override;
    STDMETHOD(OnEvent)(DWORD streamIndex, IMFMediaEvent*) override;

private:
    void Setup(const std::wstring& preferredNameHint);
    void RequestNextSample();

    std::atomic<bool> stop_{ false };
    std::atomic<ULONG> refCount_{ 1 };

    ComPtr<IMFSourceReader> reader_;
    AudioPlayer player_;
    HANDLE flushEvent_ = nullptr;
};
