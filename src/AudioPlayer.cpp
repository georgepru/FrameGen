#include "AudioPlayer.h"
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

AudioPlayer::~AudioPlayer()
{
    Stop();
}

bool AudioPlayer::Open(IMFMediaType* mediaType)
{
    Stop();
    if (!mediaType) return false;

    UINT32 channels = 0;
    UINT32 sampleRate = 0;
    UINT32 bitsPerSample = 0;
    GUID subtype = {};

    if (FAILED(mediaType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels))) return false;
    if (FAILED(mediaType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate))) return false;
    if (FAILED(mediaType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bitsPerSample))) return false;
    if (FAILED(mediaType->GetGUID(MF_MT_SUBTYPE, &subtype))) return false;

    if (channels == 0 || sampleRate == 0 || bitsPerSample == 0) return false;

    WAVEFORMATEX wfx = {};
    if (subtype == MFAudioFormat_PCM)
        wfx.wFormatTag = WAVE_FORMAT_PCM;
    else if (subtype == MFAudioFormat_Float)
        wfx.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    else
        return false;
    wfx.nChannels = static_cast<WORD>(channels);
    wfx.nSamplesPerSec = sampleRate;
    wfx.wBitsPerSample = static_cast<WORD>(bitsPerSample);
    wfx.nBlockAlign = static_cast<WORD>((wfx.nChannels * wfx.wBitsPerSample) / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT mmr = waveOutOpen(&waveOut_, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (mmr != MMSYSERR_NOERROR)
    {
        waveOut_ = nullptr;
        return false;
    }

    return true;
}

void AudioPlayer::PlaySample(IMFSample* sample)
{
    if (!sample || !waveOut_) return;

    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return;

    BYTE* src = nullptr;
    DWORD maxLen = 0;
    DWORD curLen = 0;
    if (FAILED(buf->Lock(&src, &maxLen, &curLen))) return;

    auto unlock = [&]() { buf->Unlock(); };

    if (curLen == 0)
    {
        unlock();
        return;
    }

    auto qb = std::make_unique<QueuedBuffer>();
    qb->data.assign(src, src + curLen);
    qb->hdr.lpData = reinterpret_cast<LPSTR>(qb->data.data());
    qb->hdr.dwBufferLength = curLen;

    unlock();

    std::lock_guard<std::mutex> lock(mutex_);
    CleanupDoneLocked();

    MMRESULT prep = waveOutPrepareHeader(waveOut_, &qb->hdr, sizeof(WAVEHDR));
    if (prep != MMSYSERR_NOERROR) return;

    MMRESULT wr = waveOutWrite(waveOut_, &qb->hdr, sizeof(WAVEHDR));
    if (wr != MMSYSERR_NOERROR)
    {
        waveOutUnprepareHeader(waveOut_, &qb->hdr, sizeof(WAVEHDR));
        return;
    }

    pending_.push_back(std::move(qb));
}

void AudioPlayer::CleanupDoneLocked()
{
    if (!waveOut_) return;

    for (size_t i = 0; i < pending_.size();)
    {
        auto& b = pending_[i];
        if (b->hdr.dwFlags & WHDR_DONE)
        {
            waveOutUnprepareHeader(waveOut_, &b->hdr, sizeof(WAVEHDR));
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void AudioPlayer::Stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!waveOut_) return;

    waveOutReset(waveOut_);

    for (auto& b : pending_)
    {
        waveOutUnprepareHeader(waveOut_, &b->hdr, sizeof(WAVEHDR));
    }
    pending_.clear();

    waveOutClose(waveOut_);
    waveOut_ = nullptr;
}
