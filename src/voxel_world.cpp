#include "voxel_world.h"

Chunk& VoxelWorld::getChunk(glm::ivec3 chunkPos) {
    auto it = chunks_.find(chunkPos);
    if (it != chunks_.end()) {
        return *(it->second);
    }
    assert(false && "Chunk not loaded");
    __assume(0);
}

bool VoxelWorld::isChunkLoaded(glm::ivec3 chunkPos) const {
    return chunks_.find(chunkPos) != chunks_.end();
}

bool VoxelWorld::loadChunk(glm::ivec3 chunkPos) {
    if (isChunkLoaded(chunkPos)) {
        return false;
    }
    chunks_[chunkPos] = std::make_unique<Chunk>(chunkPos);
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
    glm::ivec3 localPos = Chunk::worldToLocal(worldPos);
    auto it = chunks_.find(worldPos / Chunk::SIZE);
    if (it == chunks_.end()) {
        return BlockData{BlockType::Air, BlockOrientation::North};
    }
    return it->second->getBlock(localPos);
}

void VoxelWorld::setBlock(glm::ivec3 worldPos, BlockData blockData) {
    glm::ivec3 localPos = Chunk::worldToLocal(worldPos);
    auto& chunk = getChunk(worldPos / Chunk::SIZE);
    chunk.setBlock(localPos, blockData);
}
