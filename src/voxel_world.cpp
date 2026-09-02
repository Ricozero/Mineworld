#include "voxel_world.h"

#include <utility>

const Chunk* VoxelWorld::findChunk(glm::ivec3 chunkPos) const {
    auto it = chunks_.find(chunkPos);
    return it == chunks_.end() ? nullptr : it->second.get();
}

bool VoxelWorld::loadChunk(glm::ivec3 chunkPos, uint32_t revision, ChunkData&& data) {
    auto it = chunks_.find(chunkPos);
    if (it != chunks_.end()) {
        return it->second->applyData(revision, std::move(data));
    }

    auto chunk = std::make_unique<Chunk>(chunkPos);
    if (!chunk->applyData(revision, std::move(data))) {
        return false;
    }
    chunks_.emplace(chunkPos, std::move(chunk));
    return true;
}

bool VoxelWorld::unloadChunk(glm::ivec3 chunkPos) {
    auto it = chunks_.find(chunkPos);
    if (it == chunks_.end()) {
        return false;
    }
    chunks_.erase(it);
    return true;
}

BlockData VoxelWorld::getBlock(glm::ivec3 worldPos) const {
    const Chunk* chunk = findChunk(ChunkLayout::worldToChunk(worldPos));
    if (chunk == nullptr) {
        return BlockData{};
    }
    return chunk->getBlock(ChunkLayout::worldToLocal(worldPos));
}

BlockQueryResult VoxelWorld::queryBlock(glm::ivec3 worldPos) const {
    const Chunk* chunk = findChunk(ChunkLayout::worldToChunk(worldPos));
    if (chunk == nullptr) {
        return BlockQueryResult::Unknown;
    }
    return chunk->getBlock(ChunkLayout::worldToLocal(worldPos)).type == BlockType::Air
               ? BlockQueryResult::Empty
               : BlockQueryResult::Solid;
}
