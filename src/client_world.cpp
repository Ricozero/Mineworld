#include "client_world.h"

Chunk& ClientWorld::getChunk(glm::ivec3 chunkPos) {
    return voxelWorld_.getChunk(chunkPos);
}

bool ClientWorld::isChunkLoaded(glm::ivec3 chunkPos) const {
    return voxelWorld_.isChunkLoaded(chunkPos);
}

BlockData ClientWorld::getBlock(glm::ivec3 worldPos) const {
    return voxelWorld_.getBlock(worldPos);
}

BlockQueryResult ClientWorld::queryBlock(glm::ivec3 worldPos) const {
    return voxelWorld_.queryBlock(worldPos);
}

bool ClientWorld::loadChunk(const ChunkData& data) {
    return voxelWorld_.loadChunk(data);
}

bool ClientWorld::unloadChunk(glm::ivec3 chunkPos) {
    return voxelWorld_.unloadChunk(chunkPos);
}

entt::entity ClientWorld::createLocalPlayer(const std::string& name, uint32_t sessionId, glm::vec3 position, PlayerMode mode) {
    return actorWorld_.createLocalPlayer(name, sessionId, position, mode);
}

entt::entity ClientWorld::createRemotePlayer(const std::string& name, glm::vec3 position, PlayerMode mode) {
    return actorWorld_.createRemotePlayer(name, position, mode);
}

entt::entity ClientWorld::createRobot(const std::string& name, glm::vec3 position) {
    return actorWorld_.createRobot(name, position);
}

void ClientWorld::destroyEntity(entt::entity entity) {
    actorWorld_.destroyEntity(entity);
}

entt::entity ClientWorld::getEntityByName(const std::string& name) const {
    return actorWorld_.getEntityByName(name);
}
