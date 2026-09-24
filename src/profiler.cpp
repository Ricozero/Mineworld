#include "profiler.h"

#include <algorithm>
#include <mutex>

namespace profiling {
namespace {

std::mutex& profilerMutex() {
    static std::mutex mutex;
    return mutex;
}

template <typename Entry>
Entry& findOrAdd(std::vector<Entry>& entries, std::string_view name) {
    auto it = std::lower_bound(entries.begin(), entries.end(), name, [](const Entry& entry, std::string_view n) {
        return entry.name < n;
    });
    if (it == entries.end() || it->name != name) {
        it = entries.insert(it, Entry{std::string(name)});
    }
    return *it;
}

constexpr double kSmoothAlpha = 0.01;

double smooth(double current, double sample, double alpha) {
    return current == 0.0 ? sample : current * (1.0 - alpha) + sample * alpha;
}

}  // namespace

Profiler& Profiler::instance() {
    static Profiler profiler;
    return profiler;
}

void Profiler::recordScope(std::string_view name, double elapsedMs) {
    std::lock_guard lock(profilerMutex());

    ScopeEntry& entry = findOrAdd(scopes_, name);
    entry.curMs += elapsedMs;
}

void Profiler::finishFrame(double frameMs) {
    std::lock_guard lock(profilerMutex());
    finishFrameLocked(frameMs);
}

void Profiler::addCounter(std::string_view name, int64_t amount) {
    if (amount == 0) {
        return;
    }
    std::lock_guard lock(profilerMutex());
    CounterEntry& entry = findOrAdd(counters_, name);
    entry.curValue += amount;
}

void Profiler::setGauge(std::string_view name, double value) {
    std::lock_guard lock(profilerMutex());
    GaugeEntry& entry = findOrAdd(gauges_, name);
    entry.value = value;
    entry.avgValue = smooth(entry.avgValue, value, kSmoothAlpha);
    entry.maxValue = std::max(entry.maxValue, value);
}

Snapshot Profiler::snapshot(bool includeDetails) const {
    std::lock_guard lock(profilerMutex());

    Snapshot out;
    if (includeDetails) {
        out.scopes = scopes_;
        out.counters = counters_;
        out.gauges = gauges_;
    }
    out.frameMs = frameMs_;
    out.fps = fps_;
    out.frameIndex = frameIndex_;

    return out;
}

void Profiler::setThreadName(const char* name) {
#if defined(TRACY_ENABLE)
    TracyCSetThreadName(name);
#else
    (void)name;
#endif
}

void Profiler::finishFrameLocked(double frameMs) {
    const auto now = std::chrono::steady_clock::now();
    const bool updatePerSecond = now - counterWindowStart_ >= std::chrono::seconds(1);
    const double windowSeconds = updatePerSecond ? std::chrono::duration<double>(now - counterWindowStart_).count() : 0.0;

    frameMs_ = frameMs;
    fps_ = frameMs > 0.0 ? 1000.0 / frameMs : 0.0;
    ++frameIndex_;

    for (ScopeEntry& entry : scopes_) {
        entry.lastMs = entry.curMs;
        entry.avgMs = smooth(entry.avgMs, entry.lastMs, kSmoothAlpha);
        entry.maxMs = std::max(entry.maxMs, entry.lastMs);
        entry.curMs = 0.0;
    }
    for (CounterEntry& entry : counters_) {
        entry.lastValue = entry.curValue;
        entry.maxValue = std::max(entry.maxValue, entry.lastValue);
        entry.windowValue += entry.curValue;
        entry.curValue = 0;
        if (updatePerSecond) {
            entry.perSecondValue = static_cast<double>(entry.windowValue) / windowSeconds;
            entry.windowValue = 0;
        }
    }
    if (updatePerSecond) {
        counterWindowStart_ = now;
    }

#if defined(TRACY_ENABLE)
    TracyCPlot("Frame.ms", frameMs_);
    TracyCPlot("Frame.fps", fps_);
    TracyCFrameMark;
#endif
}

#if defined(TRACY_ENABLE)
ScopedTimer::ScopedTimer(const char* name, const ___tracy_source_location_data* sourceLocation)
    : name_(name), start_(std::chrono::steady_clock::now()) {
    tracyCtx_ = ___tracy_emit_zone_begin(sourceLocation, true);
}
#else
ScopedTimer::ScopedTimer(const char* name)
    : name_(name), start_(std::chrono::steady_clock::now()) {
}
#endif

ScopedTimer::~ScopedTimer() {
    const auto end = std::chrono::steady_clock::now();
#if defined(TRACY_ENABLE)
    TracyCZoneEnd(tracyCtx_);
#endif
    const auto elapsed = std::chrono::duration<double, std::milli>(end - start_).count();
    Profiler::instance().recordScope(name_, elapsed);
}

}  // namespace profiling
