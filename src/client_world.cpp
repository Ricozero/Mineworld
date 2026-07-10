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

bool ClientWorld::applyChunkSnapshot(glm::ivec3 chunkPos, const std::vector<BlockData>& blocks) {
    if (blocks.size() != Chunk::SIZE * Chunk::SIZE * Chunk::SIZE) {
        return false;
    }

    if (!voxelWorld_.isChunkLoaded(chunkPos)) {
        voxelWorld_.loadChunk(chunkPos);
    }

    Chunk& chunk = voxelWorld_.getChunk(chunkPos);
    size_t i = 0;
    for (int x = 0; x < Chunk::SIZE; ++x) {
        for (int y = 0; y < Chunk::SIZE; ++y) {
            for (int z = 0; z < Chunk::SIZE; ++z, ++i) {
                chunk.setBlock({x, y, z}, blocks[i]);
            }
        }
    }
    return true;
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
