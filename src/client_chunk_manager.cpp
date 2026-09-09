#include "client_chunk_manager.h"

#include <cassert>
#include <iterator>
#include <utility>

#include "log.h"
#include "voxel_world.h"

namespace {

constexpr auto kMeshRebuildDelay = std::chrono::milliseconds(50);

}  // namespace

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

    const auto [entryIt, inserted] = entries_.try_emplace(chunkPos);
    if (inserted) {
        assert(renderChunks_.size() < std::numeric_limits<uint32_t>::max());
        entryIt->second.renderIndex = static_cast<uint32_t>(renderChunks_.size());
        renderChunks_.push_back(DrawableChunk{chunkPos});
        ++layoutRevision_;
    }
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
        if (entryIt->second.meshState == MeshState::Dirty) {
            removeFromMeshQueue(entryIt->second);
        }
        if (entryIt->second.hasMesh) {
            --meshCount_;
        }
        meshPool_.release(entryIt->second.slot);
        const uint32_t renderIndex = entryIt->second.renderIndex;
        if (renderIndex != renderChunks_.size() - 1) {
            renderChunks_[renderIndex] = renderChunks_.back();
            entries_.find(renderChunks_[renderIndex].chunkPos)->second.renderIndex = renderIndex;
        }
        renderChunks_.pop_back();
        ++layoutRevision_;
        entries_.erase(entryIt);
    }

    world_.unloadChunk(chunkPos);
    coreChunks_.erase(chunkPos);

    for (const glm::ivec3& offset : kChunkFaceOffsets) {
        scheduleMeshRebuild(chunkPos + offset);
    }
    return true;
}

std::optional<ClientChunkManager::MeshTask> ClientChunkManager::takeNextMeshTask() {
    if (meshQueue_.empty()) {
        return std::nullopt;
    }

    const glm::ivec3 chunkPos = meshQueue_.front();
    Entry& entry = entries_.find(chunkPos)->second;
    if (Clock::now() - entry.dirtyTime < kMeshRebuildDelay) {
        return std::nullopt;
    }

    removeFromMeshQueue(entry);
    entry.meshState = MeshState::Building;
    return MeshTask{chunkPos, entry.meshGeneration};
}

ClientChunkManager::MeshTaskResult ClientChunkManager::completeMeshTask(const MeshTask& task, const ChunkMesh& mesh) {
    auto it = entries_.find(task.chunkPos);
    if (it == entries_.end()) {
        return MeshTaskResult::Discarded;
    }

    Entry& entry = it->second;
    if (entry.meshState != MeshState::Building || task.generation != entry.meshGeneration) {
        return MeshTaskResult::Discarded;
    }
    if (world_.findChunk(task.chunkPos) == nullptr) {
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
    DrawableChunk& renderChunk = renderChunks_[entry.renderIndex];
    const ChunkMeshBinding binding = meshPool_.binding(newSlot);
    if (renderChunk.binding.isValid() != binding.isValid()) {
        ++drawableRevision_;
    }
    renderChunk.binding = binding;
    if (renderChunk.connectivity != mesh.connectivity) {
        renderChunk.connectivity = mesh.connectivity;
        ++topologyRevision_;
    }
    if (!entry.hasMesh) {
        ++meshCount_;
    }
    entry.hasMesh = true;
    entry.meshState = MeshState::Ready;
    return MeshTaskResult::Accepted;
}

void ClientChunkManager::scheduleMeshRebuild(glm::ivec3 chunkPos) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || world_.findChunk(chunkPos) == nullptr) {
        return;
    }

    Entry& entry = it->second;
    entry.meshGeneration = nextMeshGeneration_++;
    markDirty(chunkPos, entry);
}

void ClientChunkManager::markDirty(glm::ivec3 chunkPos, Entry& entry) {
    entry.dirtyTime = Clock::now();
    entry.meshState = MeshState::Dirty;
    if (entry.inMeshQueue) {
        meshQueue_.splice(meshQueue_.end(), meshQueue_, entry.meshQueueIt);
    } else {
        meshQueue_.push_back(chunkPos);
        entry.meshQueueIt = std::prev(meshQueue_.end());
        entry.inMeshQueue = true;
    }
}

void ClientChunkManager::removeFromMeshQueue(Entry& entry) {
    assert(entry.inMeshQueue);
    meshQueue_.erase(entry.meshQueueIt);
    entry.inMeshQueue = false;
}
