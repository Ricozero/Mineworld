#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <optional>
#include <unordered_map>
#include <vector>

class ServerChunkManager {
public:
    enum class State : uint8_t {
        Unloaded,
        Queued,
        Generating,
        ReadyToCommit,
        Loaded,
        UnloadPending,
        Count,
    };

    enum class PriorityClass : uint8_t {
        LoadingCore,
        Player,
        Robot,
        Count,
    };

    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit ServerChunkManager(Clock::duration unloadDelay);

    void addRequester(glm::ivec3 chunkPos, PriorityClass priorityClass);
    void removeRequester(glm::ivec3 chunkPos, PriorityClass priorityClass, TimePoint now);
    void addRetention(glm::ivec3 chunkPos);
    void removeRetention(glm::ivec3 chunkPos, TimePoint now);

    std::vector<glm::ivec3> queuedChunks(size_t limit, const std::vector<glm::ivec3>& foci);
    std::optional<uint64_t> beginGeneration(glm::ivec3 chunkPos);
    bool finishGeneration(glm::ivec3 chunkPos, uint64_t generationId);
    bool commitLoaded(glm::ivec3 chunkPos, uint64_t generationId);
    void failGeneration(glm::ivec3 chunkPos, uint64_t generationId);

    std::vector<glm::ivec3> chunksReadyToUnload(TimePoint now);
    bool markUnloaded(glm::ivec3 chunkPos);
    void restoreLoaded(glm::ivec3 chunkPos, TimePoint now);

    size_t stateCount(State state) const { return stateCounts_[static_cast<size_t>(state)]; }
    size_t requestedChunkCount() const { return requestedCount_; }
    size_t trackedChunkCount() const { return entries_.size(); }

private:
    struct Entry {
        State state = State::Unloaded;
        bool inQueuedList = false;
        bool inUnloadPendingList = false;
        std::array<uint32_t, static_cast<size_t>(PriorityClass::Count)> requesterCounts{};
        uint32_t requesterCount = 0;
        uint32_t retentionCount = 0;
        uint64_t generationId = 0;
        TimePoint unloadTime = TimePoint::max();

        PriorityClass priorityClass() const;
    };

    Entry& getOrCreateEntry(glm::ivec3 chunkPos);
    void setState(Entry& entry, State newState);
    void enqueue(glm::ivec3 chunkPos, Entry& entry);
    void scheduleUnload(glm::ivec3 chunkPos, Entry& entry, TimePoint now);
    void onDemandGained(glm::ivec3 chunkPos, Entry& entry);
    void onDemandLost(glm::ivec3 chunkPos, Entry& entry, TimePoint now);
    void eraseIfUnused(std::unordered_map<glm::ivec3, Entry>::iterator it);

    Clock::duration unloadDelay_;
    std::unordered_map<glm::ivec3, Entry> entries_;
    std::vector<glm::ivec3> queuedList_;
    std::vector<glm::ivec3> unloadPendingList_;
    std::array<size_t, static_cast<size_t>(State::Count)> stateCounts_{};
    size_t requestedCount_ = 0;
    uint64_t nextGenerationId_ = 1;
};
