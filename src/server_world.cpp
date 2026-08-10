#include "server_world.h"

#include "chunk_generator.h"

Chunk& ServerWorld::getChunk(glm::ivec3 chunkPos) {
    return voxelWorld_.getChunk(chunkPos);
}

BlockData ServerWorld::getBlock(glm::ivec3 worldPos) const {
    return voxelWorld_.getBlock(worldPos);
}

BlockQueryResult ServerWorld::queryBlock(glm::ivec3 worldPos) const {
    return voxelWorld_.queryBlock(worldPos);
}

void ServerWorld::setBlock(glm::ivec3 worldPos, BlockData blockData) {
    voxelWorld_.setBlock(worldPos, blockData);
}

bool ServerWorld::loadChunk(glm::ivec3 chunkPos) {
    if (!isChunkInBounds(chunkPos) || voxelWorld_.isChunkLoaded(chunkPos)) {
        return false;
    }
    if (!actorWorld_.loadEntitiesInChunk(chunkPos)) {
        return false;
    }
    return voxelWorld_.applyChunkData(ChunkGenerator::generate(chunkPos));
}

bool ServerWorld::unloadChunk(glm::ivec3 chunkPos) {
    if (!voxelWorld_.isChunkLoaded(chunkPos)) {
        return false;
    }
    return actorWorld_.unloadEntitiesInChunk(chunkPos) && voxelWorld_.unloadChunk(chunkPos);
}

bool ServerWorld::isChunkInBounds(glm::ivec3 chunkPos) const {
    return ChunkGenerator::isChunkInBounds(chunkPos);
}

std::vector<glm::ivec3> ServerWorld::getLoadedChunks() const {
    return voxelWorld_.getLoadedChunks();
}

entt::entity ServerWorld::createLocalPlayer(const std::string& name, uint32_t sessionId, glm::vec3 position, PlayerMode mode) {
    return actorWorld_.createLocalPlayer(name, sessionId, position, mode);
}

entt::entity ServerWorld::createRobot(const std::string& name, glm::vec3 position) {
    return actorWorld_.createRobot(name, position);
}

void ServerWorld::destroyEntity(entt::entity entity) {
    actorWorld_.destroyEntity(entity);
}

entt::entity ServerWorld::getEntityByName(const std::string& name) const {
    return actorWorld_.getEntityByName(name);
}
