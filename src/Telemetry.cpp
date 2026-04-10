// Telemetry.cpp
#include "Telemetry.h"
#include <algorithm>
#include <cstdio>
#include <numeric>
#include <sstream>

using namespace std::chrono;

// ---------------------------------------------------------------------------
Telemetry::Telemetry(const std::wstring& logPath)
    : logPath_(logPath)
{
    records_.reserve(8192);
    captureTimes_.reserve(256);
    presentTimes_.reserve(256);
}

// ---------------------------------------------------------------------------
Telemetry::~Telemetry()
{
    Flush();
}

// ---------------------------------------------------------------------------
void Telemetry::OnCaptureFrame()
{
    auto now = Clock::now();
    {
        std::lock_guard lock(mu_);
        current_ = {};
        current_.captureArrival = now;
        captureTimes_.push_back(now);
        if (captureTimes_.size() > 120) captureTimes_.erase(captureTimes_.begin());
    }
    ++captureCount_;
}

void Telemetry::OnRifeStart()
{
    std::lock_guard lock(mu_);
    current_.rifeStart = Clock::now();
}

void Telemetry::OnRifeEnd()
{
    auto now = Clock::now();
    {
        std::lock_guard lock(mu_);
        current_.rifeEnd  = now;
        current_.inferenceMs = duration<double, std::milli>(
            current_.rifeEnd - current_.rifeStart).count();
    }
    lastInferMs_.store(current_.inferenceMs);
}

void Telemetry::OnPresent()
{
    auto now = Clock::now();
    {
        std::lock_guard lock(mu_);
        current_.presented = now;
        presentTimes_.push_back(now);
        if (presentTimes_.size() > 120) presentTimes_.erase(presentTimes_.begin());
        records_.push_back(current_);
    }
    ++presentCount_;
}

void Telemetry::OnDroppedFrame()
{
    {
        std::lock_guard lock(mu_);
        FrameRecord r = {};
        r.dropped = true;
        records_.push_back(r);
    }
    ++dropped_;
}

// ---------------------------------------------------------------------------
static double FPSFromTimes(const std::vector<TP>& times)
{
    if (times.size() < 2) return 0.0;
    double span = duration<double>(times.back() - times.front()).count();
    if (span <= 0) return 0.0;
    return (times.size() - 1) / span;
}

double Telemetry::InputFPS() const
{
    std::lock_guard lock(mu_);
    return FPSFromTimes(captureTimes_);
}

double Telemetry::OutputFPS() const
{
    std::lock_guard lock(mu_);
    return FPSFromTimes(presentTimes_);
}

double Telemetry::MeanInferMs() const
{
    std::lock_guard lock(mu_);
    if (records_.empty()) return 0.0;
    double sum = 0;
    int count  = 0;
    for (auto& r : records_)
    {
        if (!r.dropped && r.inferenceMs > 0)
        {
            sum += r.inferenceMs;
            ++count;
        }
    }
    return count > 0 ? sum / count : 0.0;
}

double Telemetry::P95InferMs() const
{
    std::lock_guard lock(mu_);
    std::vector<double> values;
    values.reserve(records_.size());
    for (auto& r : records_)
        if (!r.dropped && r.inferenceMs > 0)
            values.push_back(r.inferenceMs);
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t idx = static_cast<size_t>(values.size() * 0.95);
    idx = std::min(idx, values.size() - 1);
    return values[idx];
}

std::string Telemetry::StatsLine() const
{
    char buf[256];
    snprintf(buf, sizeof(buf),
             "In: %.1f fps  Out: %.1f fps  RIFE: %.1f ms  Dropped: %llu",
             InputFPS(), OutputFPS(),
             lastInferMs_.load(),
             (unsigned long long)dropped_.load());
    return buf;
}

// ---------------------------------------------------------------------------
void Telemetry::Flush()
{
    if (logPath_.empty() || records_.empty()) return;

    FILE* f = nullptr;
    _wfopen_s(&f, logPath_.c_str(), L"w");
    if (!f) return;

    fprintf(f, "captureArrivalUs,rifeStartUs,rifeEndUs,presentedUs,inferenceMs,dropped\n");
    auto epoch = records_.front().captureArrival;
    for (auto& r : records_)
    {
        auto toUs = [&](TP t) -> long long {
            return duration_cast<microseconds>(t - epoch).count();
        };
        fprintf(f, "%lld,%lld,%lld,%lld,%.3f,%d\n",
                r.captureArrival.time_since_epoch().count() > 0 ? toUs(r.captureArrival) : 0LL,
                r.rifeStart.time_since_epoch().count() > 0 ? toUs(r.rifeStart) : 0LL,
                r.rifeEnd.time_since_epoch().count() > 0 ? toUs(r.rifeEnd) : 0LL,
                r.presented.time_since_epoch().count() > 0 ? toUs(r.presented) : 0LL,
                r.inferenceMs,
                (int)r.dropped);
    }

    fclose(f);
}
