#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <optional>
#include <unordered_map>
#include <vector>

enum class ChunkState : uint8_t {
    Unloaded,
    Queued,
    Generating,
    ReadyToCommit,
    Loaded,
    UnloadPending,
};

enum class ChunkPriorityClass : uint8_t {
    LoadingCore,
    Player,
    Robot,
};

struct ChunkPriority {
    ChunkPriorityClass priorityClass = ChunkPriorityClass::Robot;
    int horizontalDistanceSquared = 0;
    int verticalDistance = 0;

    bool isHigherThan(const ChunkPriority& other) const;
};

struct ChunkDemand {
    uint32_t requesterCount = 0;
    uint32_t retentionCount = 0;
    std::optional<ChunkPriority> priority;

    void addRequester(ChunkPriority requesterPriority);
    void addRetention();
};

class ChunkManager {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using DemandMap = std::unordered_map<glm::ivec3, ChunkDemand>;

    struct Entry {
        ChunkState state = ChunkState::Unloaded;
        uint32_t requesterCount = 0;
        uint32_t retentionCount = 0;
        ChunkPriority priority;
        uint64_t generationId = 0;
        TimePoint unloadTime = TimePoint::max();
    };

    explicit ChunkManager(Clock::duration unloadDelay);

    void updateDemands(const DemandMap& demands, TimePoint now);

    std::vector<glm::ivec3> queuedChunks() const;
    std::optional<uint64_t> beginGeneration(glm::ivec3 chunkPos);
    bool finishGeneration(glm::ivec3 chunkPos, uint64_t generationId);
    bool commitLoaded(glm::ivec3 chunkPos, uint64_t generationId);
    void failGeneration(glm::ivec3 chunkPos, uint64_t generationId);

    std::vector<glm::ivec3> chunksReadyToUnload(TimePoint now) const;
    bool markUnloaded(glm::ivec3 chunkPos);
    void restoreLoaded(glm::ivec3 chunkPos);

    size_t stateCount(ChunkState state) const;
    size_t requestedChunkCount() const;

private:
    Clock::duration unloadDelay_;
    std::unordered_map<glm::ivec3, Entry> entries_;
    uint64_t nextGenerationId_ = 1;
};
