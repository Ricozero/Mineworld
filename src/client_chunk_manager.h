#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <list>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "chunk_data.h"
#include "chunk_mesh.h"
#include "chunk_mesh_pool.h"
#include "chunk_render_data.h"

class VoxelWorld;

class ClientChunkManager {
public:
    struct MeshTask {
        glm::ivec3 chunkPos{0};
        uint64_t generation = 0;
    };

    enum class MeshTaskResult : uint8_t {
        Accepted,
        Discarded,
        Exhausted,
    };

    explicit ClientChunkManager(VoxelWorld& world);

    void setCoreChunks(std::vector<glm::ivec3> coreChunks);
    void clearCoreChunks();
    bool areCoreChunksReady() const;

    bool upsert(glm::ivec3 chunkPos, uint32_t revision, ChunkData&& data);
    bool unload(glm::ivec3 chunkPos, uint32_t revision);

    std::optional<MeshTask> takeNextMeshTask();
    MeshTaskResult completeMeshTask(const MeshTask& task, const ChunkMesh& mesh);

    void onFrameSubmitted(uint32_t bgfxFrameNumber) { meshPool_.onFrameSubmitted(bgfxFrameNumber); }

    ChunkRenderView renderData() const { return {renderChunks_, layoutRevision_, topologyRevision_, drawableRevision_}; }

    uint16_t quadIndexBuffer() const { return meshPool_.quadIndexBuffer(); }
    size_t meshCount() const { return meshCount_; }
    size_t dirtyMeshCount() const { return meshQueue_.size(); }
    size_t meshBytesReserved() const { return meshPool_.reservedBytes(); }
    size_t meshBytesCommitted() const { return meshPool_.committedBytes(); }
    size_t meshBytesUsed() const { return meshPool_.usedBytes(); }

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using MeshQueue = std::list<glm::ivec3>;

    enum class MeshState : uint8_t {
        Dirty,
        Building,
        Ready,
    };

    struct Entry {
        MeshState meshState = MeshState::Dirty;
        TimePoint dirtyTime{};
        uint64_t meshGeneration = 0;
        bool hasMesh = false;
        bool inMeshQueue = false;
        MeshQueue::iterator meshQueueIt;
        uint32_t renderIndex = 0;
        ChunkMeshSlot slot;
    };

    void scheduleMeshRebuild(glm::ivec3 chunkPos);
    void markDirty(glm::ivec3 chunkPos, Entry& entry);
    void removeFromMeshQueue(Entry& entry);

    VoxelWorld& world_;
    std::unordered_set<glm::ivec3> coreChunks_;
    std::unordered_map<glm::ivec3, Entry> entries_;
    std::vector<DrawableChunk> renderChunks_;
    MeshQueue meshQueue_;
    ChunkMeshPool meshPool_;
    uint64_t layoutRevision_ = 0;
    uint64_t topologyRevision_ = 0;
    uint64_t drawableRevision_ = 0;
    size_t meshCount_ = 0;
    uint64_t nextMeshGeneration_ = 1;
};
