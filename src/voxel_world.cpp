#include "voxel_world.h"

Chunk& VoxelWorld::getChunk(glm::ivec3 chunkPos) {
    return *chunks_.at(chunkPos);
}

const Chunk& VoxelWorld::getChunk(glm::ivec3 chunkPos) const {
    return *chunks_.at(chunkPos);
}

bool VoxelWorld::isChunkLoaded(glm::ivec3 chunkPos) const {
    return chunks_.find(chunkPos) != chunks_.end();
}

bool VoxelWorld::loadChunk(const ChunkData& data) {
    auto it = chunks_.find(data.chunkPos);
    if (it != chunks_.end()) {
        return it->second->applyData(data);
    }

    auto chunk = std::make_unique<Chunk>(data.chunkPos);
    if (!chunk->applyData(data)) {
        return false;
    }
    chunks_.emplace(data.chunkPos, std::move(chunk));
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

ChunkData VoxelWorld::buildChunkData(glm::ivec3 chunkPos) const {
    return getChunk(chunkPos).buildData();
}

BlockData VoxelWorld::getBlock(glm::ivec3 worldPos) const {
    glm::ivec3 localPos = Chunk::worldToLocal(worldPos);
    auto it = chunks_.find(Chunk::worldToChunk(worldPos));
    if (it == chunks_.end()) {
        return BlockData{BlockType::Air, BlockOrientation::North};
    }
    return it->second->getBlock(localPos);
}

BlockQueryResult VoxelWorld::queryBlock(glm::ivec3 worldPos) const {
    const glm::ivec3 chunkPos = Chunk::worldToChunk(worldPos);
    auto it = chunks_.find(chunkPos);
    if (it == chunks_.end()) {
        return BlockQueryResult::Unknown;
    }
    return it->second->getBlock(Chunk::worldToLocal(worldPos)).type == BlockType::Air
               ? BlockQueryResult::Empty
               : BlockQueryResult::Solid;
}

void VoxelWorld::setBlock(glm::ivec3 worldPos, BlockData blockData) {
    glm::ivec3 localPos = Chunk::worldToLocal(worldPos);
    auto& chunk = getChunk(Chunk::worldToChunk(worldPos));
    chunk.setBlock(localPos, blockData);
}
