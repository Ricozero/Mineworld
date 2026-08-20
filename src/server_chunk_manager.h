#pragma once

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
    };

    enum class PriorityClass : uint8_t {
        LoadingCore,
        Player,
        Robot,
    };

    struct Priority {
        PriorityClass priorityClass = PriorityClass::Robot;
        int horizontalDistanceSquared = 0;
        int verticalDistance = 0;

        bool isHigherThan(const Priority& other) const;
    };

    struct Demand {
        uint32_t requesterCount = 0;
        uint32_t retentionCount = 0;
        std::optional<Priority> priority;

        void addRequester(Priority requesterPriority);
        void addRetention();
    };

    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using DemandMap = std::unordered_map<glm::ivec3, Demand>;

    struct Entry {
        State state = State::Unloaded;
        uint32_t requesterCount = 0;
        uint32_t retentionCount = 0;
        Priority priority;
        uint64_t generationId = 0;
        TimePoint unloadTime = TimePoint::max();
    };

    explicit ServerChunkManager(Clock::duration unloadDelay);

    void updateDemands(const DemandMap& demands, TimePoint now);

    std::vector<glm::ivec3> queuedChunks() const;
    std::optional<uint64_t> beginGeneration(glm::ivec3 chunkPos);
    bool finishGeneration(glm::ivec3 chunkPos, uint64_t generationId);
    bool commitLoaded(glm::ivec3 chunkPos, uint64_t generationId);
    void failGeneration(glm::ivec3 chunkPos, uint64_t generationId);

    std::vector<glm::ivec3> chunksReadyToUnload(TimePoint now) const;
    bool markUnloaded(glm::ivec3 chunkPos);
    void restoreLoaded(glm::ivec3 chunkPos);

    size_t stateCount(State state) const;
    size_t requestedChunkCount() const;

private:
    Clock::duration unloadDelay_;
    std::unordered_map<glm::ivec3, Entry> entries_;
    uint64_t nextGenerationId_ = 1;
};
