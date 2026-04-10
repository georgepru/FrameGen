// Telemetry.h
// Lightweight per-frame timing logger.
// Records capture→present latency, RIFE inference time, dropped frames,
// and effective input/output FPS.  Writes a CSV log on destruction.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
using TP    = Clock::time_point;

struct FrameRecord
{
    TP       captureArrival;    // when the frame arrived from MF callback
    TP       rifeStart;         // when RIFE inference started
    TP       rifeEnd;           // when RIFE inference finished
    TP       presented;         // when Present() was called
    double   inferenceMs = 0;   // rifeEnd - rifeStart in ms
    bool     dropped     = false;
};

class Telemetry
{
public:
    explicit Telemetry(const std::wstring& logPath);
    ~Telemetry();

    // Record events
    void OnCaptureFrame();
    void OnRifeStart();
    void OnRifeEnd();
    void OnPresent();
    void OnDroppedFrame();

    // Snapshot stats (thread-safe)
    double InputFPS()     const;
    double OutputFPS()    const;
    double MeanInferMs()  const;
    double P95InferMs()   const;
    uint64_t DroppedFrames() const { return dropped_.load(); }

    // Copy current stats into a text buffer for overlay rendering.
    std::string StatsLine() const;

private:
    void Flush();

    std::wstring logPath_;

    mutable std::mutex mu_;
    std::vector<FrameRecord> records_;
    FrameRecord current_;

    // Rolling windows for FPS calculation
    std::vector<TP> captureTimes_;
    std::vector<TP> presentTimes_;

    std::atomic<uint64_t> dropped_{ 0 };
    std::atomic<uint64_t> captureCount_{ 0 };
    std::atomic<uint64_t> presentCount_{ 0 };

    // Last completed inference duration (for fast read in StatsLine)
    std::atomic<double> lastInferMs_{ 0.0 };
};
