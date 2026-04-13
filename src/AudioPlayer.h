#pragma once
#include "Common.h"
#include <mmsystem.h>

class AudioPlayer
{
public:
    AudioPlayer() = default;
    ~AudioPlayer();

    bool Open(IMFMediaType* mediaType);
    void PlaySample(IMFSample* sample);
    void Stop();

private:
    struct QueuedBuffer
    {
        WAVEHDR hdr = {};
        std::vector<BYTE> data;
    };

    void CleanupDoneLocked();

    HWAVEOUT waveOut_ = nullptr;
    std::mutex mutex_;
    std::vector<std::unique_ptr<QueuedBuffer>> pending_;
};
