#include "profiler.h"

#include <algorithm>

namespace profiling {

Profiler& Profiler::instance() {
    static Profiler profiler;
    return profiler;
}

void Profiler::record(std::string_view name, double elapsedMs) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [name](const Entry& entry) {
        return entry.name == name;
    });

    if (it == entries_.end()) {
        entries_.push_back(Entry{std::string(name), elapsedMs, elapsedMs});
        it = entries_.end() - 1;
    } else {
        it->lastMs = elapsedMs;
        it->averageMs = it->averageMs * 0.95 + elapsedMs * 0.05;
    }

    if (name == "Frame.Total") {
        frameMs_ = elapsedMs;
        fps_ = elapsedMs > 0.0 ? 1000.0 / elapsedMs : 0.0;
    }
}

Snapshot Profiler::snapshot() const {
    Snapshot out;
    out.entries = entries_;
    out.frameMs = frameMs_;
    out.fps = fps_;
    return out;
}

ScopedTimer::ScopedTimer(std::string_view name)
    : name_(name), start_(std::chrono::steady_clock::now()) {
}

ScopedTimer::~ScopedTimer() {
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<double, std::milli>(end - start_).count();
    Profiler::instance().record(name_, elapsed);
}

}  // namespace profiling
