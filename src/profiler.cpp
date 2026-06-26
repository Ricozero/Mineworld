#include "profiler.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace profiling {
namespace {

std::mutex& profilerMutex() {
    static std::mutex mutex;
    return mutex;
}

template <typename Entry>
Entry& findOrAdd(std::vector<Entry>& entries, std::string_view name) {
    auto it = std::find_if(entries.begin(), entries.end(), [name](const Entry& entry) {
        return entry.name == name;
    });
    if (it == entries.end()) {
        entries.push_back(Entry{std::string(name)});
        it = entries.end() - 1;
    }
    return *it;
}

template <typename Entry>
Entry& findOrAddSorted(std::vector<Entry>& entries, std::string_view name) {
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

#if defined(TRACY_ENABLE)
struct TracySourceKey {
    std::string name;
    std::string file;
    std::string function;
    uint32_t line = 0;

    bool operator==(const TracySourceKey& rhs) const {
        return line == rhs.line && name == rhs.name && file == rhs.file && function == rhs.function;
    }
};

struct TracySourceKeyHash {
    size_t operator()(const TracySourceKey& key) const {
        size_t seed = std::hash<std::string>{}(key.name);
        seed ^= std::hash<std::string>{}(key.file) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        seed ^= std::hash<std::string>{}(key.function) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        seed ^= std::hash<uint32_t>{}(key.line) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};

uint64_t tracySourceLocation(std::string_view name, const char* file, uint32_t line, const char* function) {
    static std::mutex mutex;
    static std::unordered_map<TracySourceKey, uint64_t, TracySourceKeyHash> cache;

    TracySourceKey key{
        std::string(name),
        file ? file : "profiler.cpp",
        function ? function : "profiling::ScopedTimer",
        line,
    };

    std::lock_guard lock(mutex);
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }

    auto [insertedIt, _] = cache.emplace(std::move(key), 0);
    const TracySourceKey& stored = insertedIt->first;
    const uint64_t sourceLocation = ___tracy_alloc_srcloc_name(
        stored.line,
        stored.file.c_str(),
        stored.file.size(),
        stored.function.c_str(),
        stored.function.size(),
        stored.name.c_str(),
        stored.name.size(),
        0);
    insertedIt->second = sourceLocation;
    return sourceLocation;
}
#endif

}  // namespace

Profiler& Profiler::instance() {
    static Profiler profiler;
    return profiler;
}

void Profiler::recordScope(std::string_view name, double elapsedMs) {
    std::lock_guard lock(profilerMutex());

    ScopeEntry& entry = findOrAddSorted(scopes_, name);
    entry.curMs += elapsedMs;

    if (name == "Frame.Total") {
        finishFrameLocked(elapsedMs);
    }
}

void Profiler::addCounter(std::string_view name, int64_t amount) {
    std::lock_guard lock(profilerMutex());
    CounterEntry& entry = findOrAddSorted(counters_, name);
    entry.curValue += amount;
    entry.totalValue += amount;
}

void Profiler::setGauge(std::string_view name, double value) {
    std::lock_guard lock(profilerMutex());
    GaugeEntry& entry = findOrAddSorted(gauges_, name);
    entry.value = value;
    entry.avgValue = smooth(entry.avgValue, value, kSmoothAlpha);
    entry.maxValue = std::max(entry.maxValue, value);
}

Snapshot Profiler::snapshot() const {
    std::lock_guard lock(profilerMutex());

    Snapshot out;
    out.scopes = scopes_;
    out.counters = counters_;
    out.gauges = gauges_;
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
    frameMs_ = frameMs;
    fps_ = frameMs > 0.0 ? 1000.0 / frameMs : 0.0;
    ++frameIndex_;

    for (ScopeEntry& entry : scopes_) {
        entry.lastMs = entry.curMs;
        entry.avgMs = smooth(entry.avgMs, entry.lastMs, kSmoothAlpha);
        entry.maxMs = std::max(entry.maxMs, entry.lastMs);
    }
    for (CounterEntry& entry : counters_) {
        entry.lastValue = entry.curValue;
        entry.avgValue = smooth(entry.avgValue, static_cast<double>(entry.lastValue), kSmoothAlpha);
        entry.maxValue = std::max(entry.maxValue, entry.lastValue);
    }

#if defined(TRACY_ENABLE)
    TracyCPlot("Frame.ms", frameMs_);
    TracyCPlot("Frame.fps", fps_);
    TracyCFrameMark;
#endif

    for (ScopeEntry& entry : scopes_) {
        entry.curMs = 0.0;
    }
    for (CounterEntry& entry : counters_) {
        entry.curValue = 0;
    }
}

ScopedTimer::ScopedTimer(std::string_view name, const char* file, uint32_t line, const char* function)
    : name_(name), start_(std::chrono::steady_clock::now()) {
#if defined(TRACY_ENABLE)
    const uint64_t sourceLocation = tracySourceLocation(name_, file, line, function);
    tracyCtx_ = ___tracy_emit_zone_begin_alloc_callstack(sourceLocation, TRACY_CALLSTACK, true);
#else
    (void)file;
    (void)line;
    (void)function;
#endif
}

ScopedTimer::~ScopedTimer() {
    const auto end = std::chrono::steady_clock::now();
#if defined(TRACY_ENABLE)
    TracyCZoneEnd(tracyCtx_);
#endif
    const auto elapsed = std::chrono::duration<double, std::milli>(end - start_).count();
    Profiler::instance().recordScope(name_, elapsed);
}

}  // namespace profiling
