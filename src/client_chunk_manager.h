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

class ClientWorld;

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

    explicit ClientChunkManager(ClientWorld& world);

    void setCoreChunks(std::vector<glm::ivec3> coreChunks);
    void clearCoreChunks();
    bool areCoreChunksReady() const;

    bool upsert(const ChunkData& data);
    bool unload(glm::ivec3 chunkPos, uint32_t revision);

    std::optional<MeshTask> takeNextMeshTask(const MeshFocus& focus);
    bool completeMeshTask(const MeshTask& task, ChunkMesh mesh);

    const ChunkMesh* getMesh(glm::ivec3 chunkPos) const;
    size_t meshCount() const { return meshCount_; }
    size_t dirtyMeshCount() const;

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
        std::optional<ChunkMesh> mesh;
    };

    void scheduleMeshRebuild(glm::ivec3 chunkPos);
    bool isCoreChunk(glm::ivec3 chunkPos) const;

    ClientWorld& world_;
    std::unordered_set<glm::ivec3> coreChunks_;
    std::unordered_map<glm::ivec3, Entry> entries_;
    size_t meshCount_ = 0;
    uint64_t nextMeshOrder_ = 1;
};
