#include "world.h"

World::World() {
}

World::~World() = default;

Chunk& World::getChunk(glm::ivec3 chunkPos) {
    return voxelWorld_.getChunk(chunkPos);
}

BlockData World::getBlock(glm::ivec3 worldPos) const {
    return voxelWorld_.getBlock(worldPos);
}

void World::setBlock(glm::ivec3 worldPos, BlockData blockData) {
    voxelWorld_.setBlock(worldPos, blockData);
}

bool World::loadChunk(glm::ivec3 chunkPos) {
    if (voxelWorld_.isChunkLoaded(chunkPos)) {
        return false;
    }
    return actorWorld_.loadEntitiesInChunk(chunkPos) && voxelWorld_.loadChunk(chunkPos);
}

bool World::unloadChunk(glm::ivec3 chunkPos) {
    if (!voxelWorld_.isChunkLoaded(chunkPos)) {
        return false;
    }
    return actorWorld_.unloadEntitiesInChunk(chunkPos) && voxelWorld_.unloadChunk(chunkPos);
}

entt::entity World::createPlayer(const std::string& name, glm::vec3 position) {
    return actorWorld_.createPlayer(name, position);
}

void World::destroyEntity(entt::entity entity) {
    actorWorld_.destroyEntity(entity);
}

entt::entity World::getEntityByName(const std::string& name) const {
    return actorWorld_.getEntityByName(name);
}
