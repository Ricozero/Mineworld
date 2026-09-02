#include "client_chunk_manager.h"

#include <algorithm>
#include <array>
#include <utility>

#include "helper.h"
#include "log.h"
#include "voxel_world.h"

ClientChunkManager::ClientChunkManager(VoxelWorld& world) : world_(world) {
    if (!meshPool_.initialize()) {
        logging::error("Chunk mesh pool failed to initialize");
    }
}

void ClientChunkManager::setCoreChunks(std::vector<glm::ivec3> coreChunks) {
    coreChunks_.clear();
    coreChunks_.insert(coreChunks.begin(), coreChunks.end());
}

void ClientChunkManager::clearCoreChunks() {
    coreChunks_.clear();
}

bool ClientChunkManager::areCoreChunksReady() const {
    for (const glm::ivec3& chunkPos : coreChunks_) {
        auto it = entries_.find(chunkPos);
        if (it == entries_.end() || it->second.meshState != MeshState::Ready) {
            return false;
        }
    }
    return true;
}

bool ClientChunkManager::upsert(glm::ivec3 chunkPos, uint32_t revision, ChunkData&& data) {
    if (!world_.loadChunk(chunkPos, revision, std::move(data))) {
        return false;
    }

    entries_.try_emplace(chunkPos);
    scheduleMeshRebuild(chunkPos);
    for (const glm::ivec3& offset : kChunkFaceOffsets) {
        scheduleMeshRebuild(chunkPos + offset);
    }
    return true;
}

bool ClientChunkManager::unload(glm::ivec3 chunkPos, uint32_t revision) {
    const Chunk* chunk = world_.findChunk(chunkPos);
    if (chunk != nullptr && revision < chunk->getRevision()) {
        return false;
    }

    auto entryIt = entries_.find(chunkPos);
    if (entryIt != entries_.end()) {
        if (entryIt->second.hasMesh) {
            --meshCount_;
        }
        meshPool_.release(entryIt->second.slot);
        entries_.erase(entryIt);
    }

    world_.unloadChunk(chunkPos);
    coreChunks_.erase(chunkPos);

    for (const glm::ivec3& offset : kChunkFaceOffsets) {
        scheduleMeshRebuild(chunkPos + offset);
    }
    return true;
}

std::optional<ClientChunkManager::MeshTask> ClientChunkManager::takeNextMeshTask(const MeshFocus& focus) {
    struct MeshPriority {
        int coreRank;
        int visibilityRank;
        int distanceSq;
        uint64_t inverseMeshOrder;
        std::array<int, 3> chunkPos;
        auto operator<=>(const MeshPriority&) const = default;
    };

    const auto priorityOf = [&](glm::ivec3 chunkPos, const Entry& entry) {
        const glm::vec3 offset = glm::vec3(chunkPos - focus.centerChunk);
        return MeshPriority{
            isCoreChunk(chunkPos) ? 0 : 1,
            glm::dot(offset, focus.forward) >= 0.0f ? 0 : 1,
            ivec3DistanceSq(chunkPos, focus.centerChunk),
            ~entry.meshOrder,
            {chunkPos.x, chunkPos.y, chunkPos.z},
        };
    };

    auto best = entries_.end();
    MeshPriority bestPriority{};
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->second.meshState != MeshState::Dirty || world_.findChunk(it->first) == nullptr) {
            continue;
        }

        const MeshPriority priority = priorityOf(it->first, it->second);
        if (best == entries_.end() || priority < bestPriority) {
            best = it;
            bestPriority = priority;
        }
    }
    if (best == entries_.end()) {
        return std::nullopt;
    }

    Entry& entry = best->second;
    entry.meshState = MeshState::Building;
    return MeshTask{best->first, entry.meshGeneration};
}

ClientChunkManager::MeshTaskResult ClientChunkManager::completeMeshTask(const MeshTask& task, const ChunkMesh& mesh) {
    auto it = entries_.find(task.chunkPos);
    if (it == entries_.end()) {
        return MeshTaskResult::Discarded;
    }

    Entry& entry = it->second;
    if (entry.meshState != MeshState::Building) {
        return MeshTaskResult::Discarded;
    }
    if (task.generation != entry.meshGeneration || world_.findChunk(task.chunkPos) == nullptr) {
        entry.meshState = MeshState::Dirty;
        return MeshTaskResult::Discarded;
    }

    ChunkMeshSlot newSlot;
    if (!meshPool_.upload(mesh.vertices, newSlot)) {
        entry.meshState = MeshState::Dirty;
        return MeshTaskResult::Exhausted;
    }

    meshPool_.release(entry.slot);
    entry.slot = newSlot;
    entry.faceConnectivity = mesh.faceConnectivity;
    if (!entry.hasMesh) {
        ++meshCount_;
    }
    entry.hasMesh = true;
    entry.meshState = MeshState::Ready;
    return MeshTaskResult::Accepted;
}

std::optional<ChunkMeshBinding> ClientChunkManager::meshBinding(glm::ivec3 chunkPos) const {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || !it->second.slot.isValid()) {
        return std::nullopt;
    }
    return meshPool_.binding(it->second.slot);
}

ChunkFaceConnectivity ClientChunkManager::faceConnectivity(glm::ivec3 chunkPos) const {
    auto it = entries_.find(chunkPos);
    return it != entries_.end() ? it->second.faceConnectivity : ~0u;
}

size_t ClientChunkManager::dirtyMeshCount() const {
    return static_cast<size_t>(std::count_if(entries_.begin(), entries_.end(), [](const auto& item) {
        return item.second.meshState == MeshState::Dirty;
    }));
}

void ClientChunkManager::scheduleMeshRebuild(glm::ivec3 chunkPos) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || world_.findChunk(chunkPos) == nullptr) {
        return;
    }

    Entry& entry = it->second;
    ++entry.meshGeneration;
    entry.meshOrder = nextMeshOrder_++;
    if (entry.meshState != MeshState::Building) {
        entry.meshState = MeshState::Dirty;
    }
}

bool ClientChunkManager::isCoreChunk(glm::ivec3 chunkPos) const {
    return coreChunks_.contains(chunkPos);
}
