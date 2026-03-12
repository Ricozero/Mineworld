#include "voxel_world.h"

#include <spdlog/spdlog.h>

#include <glm/gtx/string_cast.hpp>

#include "helper.h"

Chunk& VoxelWorld::getChunk(glm::ivec3 chunkPos) {
    auto it = chunks_.find(chunkPos);
    if (it != chunks_.end()) {
        return *(it->second);
    }
    crash("Chunk not loaded at {}", glm::to_string(chunkPos));
}

const Chunk& VoxelWorld::getChunk(glm::ivec3 chunkPos) const {
    auto it = chunks_.find(chunkPos);
    if (it != chunks_.end()) {
        return *(it->second);
    }
    crash("Chunk not loaded at {}", glm::to_string(chunkPos));
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
        spdlog::error("Attempted to get block at {}", glm::to_string(worldPos));
        return BlockData{BlockType::Air, BlockOrientation::North};
    }
    return it->second->getBlock(localPos);
}

BlockData VoxelWorld::getBlockOrAir(glm::ivec3 worldPos) const {
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

bool VoxelWorld::setBlockIfChunkLoaded(glm::ivec3 worldPos, BlockData blockData) {
    glm::ivec3 chunkPos = worldPos / Chunk::SIZE;
    auto it = chunks_.find(chunkPos);
    if (it == chunks_.end()) {
        return false;
    }
    it->second->setBlock(Chunk::worldToLocal(worldPos), blockData);
    return true;
}

std::vector<glm::ivec3> VoxelWorld::getLoadedChunks() const {
    std::vector<glm::ivec3> loaded;
    loaded.reserve(chunks_.size());
    for (const auto& [chunkPos, _] : chunks_) {
        loaded.push_back(chunkPos);
    }
    return loaded;
}
