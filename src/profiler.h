#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace profiling {

struct Entry {
    std::string name;
    double lastMs = 0.0;
    double averageMs = 0.0;
};

struct Snapshot {
    std::vector<Entry> entries;
    double frameMs = 0.0;
    double fps = 0.0;
};

class Profiler {
public:
    static Profiler& instance();

    void record(std::string_view name, double elapsedMs);
    Snapshot snapshot() const;

private:
    std::vector<Entry> entries_;
    double frameMs_ = 0.0;
    double fps_ = 0.0;
};

class ScopedTimer {
public:
    explicit ScopedTimer(std::string_view name);
    ~ScopedTimer();

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    std::string name_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace profiling