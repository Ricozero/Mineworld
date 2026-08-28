#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "chunk.h"
#include "chunk_mesh.h"
#include "chunk_mesh_pool.h"

class VoxelWorld;

class ClientChunkManager {
public:
    struct MeshTask {
        glm::ivec3 chunkPos{0};
        uint64_t generation = 0;
    };

    struct MeshFocus {
        glm::ivec3 centerChunk{0};
        glm::vec3 forward{0.0f};
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

    bool upsert(const ChunkData& data);
    bool unload(glm::ivec3 chunkPos, uint32_t revision);

    std::optional<MeshTask> takeNextMeshTask(const MeshFocus& focus);
    MeshTaskResult completeMeshTask(const MeshTask& task, const ChunkMesh& mesh);

    void onFrameSubmitted(uint32_t bgfxFrameNumber) { meshPool_.onFrameSubmitted(bgfxFrameNumber); }

    std::optional<ChunkMeshBinding> meshBinding(glm::ivec3 chunkPos) const;
    ChunkFaceConnectivity faceConnectivity(glm::ivec3 chunkPos) const;
    uint16_t quadIndexBuffer() const { return meshPool_.quadIndexBuffer(); }
    size_t meshCount() const { return meshCount_; }
    size_t dirtyMeshCount() const;
    size_t meshBytesReserved() const { return meshPool_.reservedBytes(); }
    size_t meshBytesCommitted() const { return meshPool_.committedBytes(); }
    size_t meshBytesUsed() const { return meshPool_.usedBytes(); }

private:
    enum class MeshState : uint8_t {
        Dirty,
        Building,
        Ready,
    };

    struct Entry {
        MeshState meshState = MeshState::Dirty;
        uint64_t meshOrder = 0;
        uint64_t meshGeneration = 0;
        bool hasMesh = false;
        ChunkFaceConnectivity faceConnectivity = ~0u;
        ChunkMeshSlot slot;
    };

    void scheduleMeshRebuild(glm::ivec3 chunkPos);
    bool isCoreChunk(glm::ivec3 chunkPos) const;

    VoxelWorld& world_;
    std::unordered_set<glm::ivec3> coreChunks_;
    std::unordered_map<glm::ivec3, Entry> entries_;
    ChunkMeshPool meshPool_;
    size_t meshCount_ = 0;
    uint64_t nextMeshOrder_ = 1;
};
