#include "server_world.h"

#include "profiler.h"

namespace {

constexpr glm::ivec3 kWorldMin{-1024, -256, -1024};
constexpr glm::ivec3 kWorldMax{1024, 256, 1024};

}  // namespace

Chunk& ServerWorld::getChunk(glm::ivec3 chunkPos) {
    return voxelWorld_.getChunk(chunkPos);
}

BlockData ServerWorld::getBlock(glm::ivec3 worldPos) const {
    return voxelWorld_.getBlock(worldPos);
}

void ServerWorld::setBlock(glm::ivec3 worldPos, BlockData blockData) {
    voxelWorld_.setBlock(worldPos, blockData);
}

bool ServerWorld::loadChunk(glm::ivec3 chunkPos) {
    if (!isChunkInBounds(chunkPos)) {
        return false;
    }
    if (voxelWorld_.isChunkLoaded(chunkPos)) {
        return false;
    }
    if (!actorWorld_.loadEntitiesInChunk(chunkPos) || !voxelWorld_.loadChunk(chunkPos)) {
        return false;
    }
    generateChunk(voxelWorld_.getChunk(chunkPos));
    return true;
}

bool ServerWorld::unloadChunk(glm::ivec3 chunkPos) {
    if (!voxelWorld_.isChunkLoaded(chunkPos)) {
        return false;
    }
    return actorWorld_.unloadEntitiesInChunk(chunkPos) && voxelWorld_.unloadChunk(chunkPos);
}

bool ServerWorld::isChunkInBounds(glm::ivec3 chunkPos) const {
    const glm::ivec3 minChunk = Chunk::worldToChunk(kWorldMin);
    const glm::ivec3 maxChunk = Chunk::worldToChunk(kWorldMax);
    return chunkPos.x >= minChunk.x && chunkPos.x < maxChunk.x &&
           chunkPos.y >= minChunk.y && chunkPos.y < maxChunk.y &&
           chunkPos.z >= minChunk.z && chunkPos.z < maxChunk.z;
}

std::vector<glm::ivec3> ServerWorld::getLoadedChunks() const {
    return voxelWorld_.getLoadedChunks();
}

entt::entity ServerWorld::createPlayer(const std::string& name, uint32_t sessionId, glm::vec3 position, PlayerMode mode) {
    return actorWorld_.createPlayer(name, sessionId, position, mode);
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

void ServerWorld::generateChunk(Chunk& chunk) const {
    MW_PROFILE_SCOPE("Server.GenerateChunk");
    MW_PROFILE_COUNTER("World.ChunksGenerated", 1);

    for (int x = 0; x < Chunk::SIZE; ++x) {
        for (int y = 0; y < Chunk::SIZE; ++y) {
            for (int z = 0; z < Chunk::SIZE; ++z) {
                const glm::ivec3 localPos(x, y, z);
                chunk.setBlock(localPos, generateBlock(chunk.localToWorld(localPos)));
            }
        }
    }
}

BlockData ServerWorld::generateBlock(glm::ivec3 worldPos) const {
    if (!isBlockInBounds(worldPos) || worldPos.y > 0) {
        return BlockData{BlockType::Air, BlockOrientation::North};
    }
    if (worldPos.y == 0) {
        return BlockData{BlockType::Grass, BlockOrientation::North};
    }
    if (worldPos.y >= -3) {
        return BlockData{BlockType::Dirt, BlockOrientation::North};
    }
    return BlockData{BlockType::Stone, BlockOrientation::North};
}

bool ServerWorld::isBlockInBounds(glm::ivec3 worldPos) const {
    return worldPos.x >= kWorldMin.x && worldPos.x < kWorldMax.x &&
           worldPos.y >= kWorldMin.y && worldPos.y < kWorldMax.y &&
           worldPos.z >= kWorldMin.z && worldPos.z < kWorldMax.z;
}
