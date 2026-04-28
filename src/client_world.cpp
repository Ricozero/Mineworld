#include "client_world.h"

Chunk& ClientWorld::getChunk(glm::ivec3 chunkPos) {
    return voxelWorld_.getChunk(chunkPos);
}

BlockData ClientWorld::getBlock(glm::ivec3 worldPos) const {
    return voxelWorld_.getBlockOrAir(worldPos);
}

bool ClientWorld::loadChunk(glm::ivec3 chunkPos) {
    return voxelWorld_.loadChunk(chunkPos);
}

bool ClientWorld::unloadChunk(glm::ivec3 chunkPos) {
    return voxelWorld_.unloadChunk(chunkPos);
}

void ClientWorld::applyBlockSnapshot(glm::ivec3 worldPos, BlockData blockData) {
    const glm::ivec3 chunkPos = worldPos / Chunk::SIZE;
    if (!voxelWorld_.isChunkLoaded(chunkPos)) {
        voxelWorld_.loadChunk(chunkPos);
    }
    voxelWorld_.setBlockIfChunkLoaded(worldPos, blockData);
}

entt::entity ClientWorld::createPlayer(const std::string& name, glm::vec3 position) {
    return actorWorld_.createPlayer(name, position);
}

void ClientWorld::destroyEntity(entt::entity entity) {
    actorWorld_.destroyEntity(entity);
}

entt::entity ClientWorld::getEntityByName(const std::string& name) const {
    return actorWorld_.getEntityByName(name);
}
