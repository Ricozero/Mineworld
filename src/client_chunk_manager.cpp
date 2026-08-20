#include "client_chunk_manager.h"

#include <algorithm>
#include <array>

#include "client_world.h"
#include "helper.h"

ClientChunkManager::ClientChunkManager(ClientWorld& world) : world_(world) {}

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

bool ClientChunkManager::upsert(const ChunkData& data) {
    if (!world_.loadChunk(data)) {
        return false;
    }

    entries_.try_emplace(data.chunkPos);
    scheduleMeshRebuild(data.chunkPos);
    for (const glm::ivec3& offset : kChunkFaceOffsets) {
        scheduleMeshRebuild(data.chunkPos + offset);
    }
    return true;
}

bool ClientChunkManager::unload(glm::ivec3 chunkPos, uint32_t revision) {
    if (world_.isChunkLoaded(chunkPos) && revision < world_.getChunk(chunkPos).getRevision()) {
        return false;
    }

    auto entryIt = entries_.find(chunkPos);
    if (entryIt != entries_.end()) {
        if (entryIt->second.mesh) {
            --meshCount_;
        }
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
        if (it->second.meshState != MeshState::Dirty || !world_.isChunkLoaded(it->first)) {
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

bool ClientChunkManager::completeMeshTask(const MeshTask& task, ChunkMesh mesh) {
    auto it = entries_.find(task.chunkPos);
    if (it == entries_.end()) {
        return false;
    }

    Entry& entry = it->second;
    if (entry.meshState != MeshState::Building) {
        return false;
    }
    if (task.generation != entry.meshGeneration || !world_.isChunkLoaded(task.chunkPos)) {
        entry.meshState = MeshState::Dirty;
        return false;
    }

    if (!entry.mesh) {
        ++meshCount_;
    }
    entry.mesh = std::move(mesh);
    entry.meshState = MeshState::Ready;
    return true;
}

const ChunkMesh* ClientChunkManager::getMesh(glm::ivec3 chunkPos) const {
    auto it = entries_.find(chunkPos);
    return it != entries_.end() && it->second.mesh ? &*it->second.mesh : nullptr;
}

size_t ClientChunkManager::dirtyMeshCount() const {
    return static_cast<size_t>(std::count_if(entries_.begin(), entries_.end(), [](const auto& item) {
        return item.second.meshState == MeshState::Dirty;
    }));
}

void ClientChunkManager::scheduleMeshRebuild(glm::ivec3 chunkPos) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || !world_.isChunkLoaded(chunkPos)) {
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
