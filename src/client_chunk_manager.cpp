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
    size_t bestSlot = 0;
    MeshPriority bestPriority{};
    size_t kept = 0;
    for (const glm::ivec3 chunkPos : dirtyChunks_) {
        auto it = entries_.find(chunkPos);
        if (it == entries_.end()) {
            continue;
        }
        if (it->second.meshState != MeshState::Dirty) {
            it->second.inDirtyList = false;
            continue;
        }

        if (world_.findChunk(chunkPos) != nullptr) {
            const MeshPriority priority = priorityOf(chunkPos, it->second);
            if (best == entries_.end() || priority < bestPriority) {
                best = it;
                bestSlot = kept;
                bestPriority = priority;
            }
        }
        dirtyChunks_[kept++] = chunkPos;
    }
    dirtyChunks_.resize(kept);
    if (best == entries_.end()) {
        return std::nullopt;
    }

    dirtyChunks_[bestSlot] = dirtyChunks_.back();
    dirtyChunks_.pop_back();

    Entry& entry = best->second;
    entry.inDirtyList = false;
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
        markDirty(task.chunkPos, entry);
        return MeshTaskResult::Discarded;
    }

    ChunkMeshSlot newSlot;
    if (!meshPool_.upload(mesh.vertices, newSlot)) {
        markDirty(task.chunkPos, entry);
        return MeshTaskResult::Exhausted;
    }

    meshPool_.release(entry.slot);
    entry.slot = newSlot;
    entry.binding = meshPool_.binding(newSlot);
    entry.faceConnectivity = mesh.faceConnectivity;
    if (!entry.hasMesh) {
        ++meshCount_;
    }
    entry.hasMesh = true;
    entry.meshState = MeshState::Ready;
    return MeshTaskResult::Accepted;
}

void ClientChunkManager::collectChunks(std::vector<DrawableChunk>& out) const {
    out.clear();
    out.reserve(entries_.size());
    for (const auto& [chunkPos, entry] : entries_) {
        out.push_back(DrawableChunk{chunkPos, entry.faceConnectivity, entry.binding});
    }
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
        markDirty(chunkPos, entry);
    }
}

void ClientChunkManager::markDirty(glm::ivec3 chunkPos, Entry& entry) {
    entry.meshState = MeshState::Dirty;
    if (!entry.inDirtyList) {
        entry.inDirtyList = true;
        dirtyChunks_.push_back(chunkPos);
    }
}

bool ClientChunkManager::isCoreChunk(glm::ivec3 chunkPos) const {
    return coreChunks_.contains(chunkPos);
}
