#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#if defined(TRACY_ENABLE)
#include <tracy/TracyC.h>
#endif

#define MW_PROFILE_JOIN_INNER(a, b) a##b
#define MW_PROFILE_JOIN(a, b) MW_PROFILE_JOIN_INNER(a, b)
#define MW_PROFILE_SCOPE(name) ::profiling::ScopedTimer MW_PROFILE_JOIN(mwProfileScope_, __LINE__)(name, __FILE__, __LINE__, __FUNCTION__)
#define MW_PROFILE_FUNCTION() MW_PROFILE_SCOPE(__FUNCTION__)
#define MW_PROFILE_COUNTER(name, amount) ::profiling::Profiler::instance().addCounter(name, amount)
#define MW_PROFILE_GAUGE(name, value) ::profiling::Profiler::instance().setGauge(name, value)

namespace profiling {

struct ScopeEntry {
    std::string name;
    double lastMs = 0.0;
    double curMs = 0.0;
    double avgMs = 0.0;
    double maxMs = 0.0;
    int64_t lastCalls = 0;
    int64_t curCalls = 0;
    double avgCalls = 0;
    int64_t maxCalls = 0;
};

struct CounterEntry {
    std::string name;
    int64_t lastValue = 0;
    int64_t curValue = 0;
    double avgValue = 0.0;
    int64_t maxValue = 0;
    int64_t totalValue = 0;
};

struct GaugeEntry {
    std::string name;
    double value = 0.0;
    double avgValue = 0.0;
    double maxValue = 0.0;
};

struct Snapshot {
    std::vector<ScopeEntry> scopes;
    std::vector<CounterEntry> counters;
    std::vector<GaugeEntry> gauges;
    double frameMs = 0.0;
    double fps = 0.0;
    int64_t frameIndex = 0;
};

class Profiler {
public:
    static Profiler& instance();

    void recordScope(std::string_view name, double elapsedMs);
    void addCounter(std::string_view name, int64_t amount = 1);
    void setGauge(std::string_view name, double value);
    Snapshot snapshot() const;
    void setThreadName(const char* name);

private:
    void finishFrameLocked(double frameMs);

    std::vector<ScopeEntry> scopes_;
    std::vector<CounterEntry> counters_;
    std::vector<GaugeEntry> gauges_;
    double frameMs_ = 0.0;
    double fps_ = 0.0;
    int64_t frameIndex_ = 0;
};

class ScopedTimer {
public:
    ScopedTimer(std::string_view name, const char* file, uint32_t line, const char* function);
    ~ScopedTimer();

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    std::string name_;
    std::chrono::steady_clock::time_point start_;
#if defined(TRACY_ENABLE)
    TracyCZoneCtx tracyCtx_{};
#endif
};

}  // namespace profiling
