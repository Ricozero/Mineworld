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
    setBlockServer(worldPos, blockData);
}

bool World::loadChunk(glm::ivec3 chunkPos) {
    return loadChunkServer(chunkPos);
}

bool World::unloadChunk(glm::ivec3 chunkPos) {
    return unloadChunkServer(chunkPos);
}

bool World::loadChunkServer(glm::ivec3 chunkPos) {
    if (voxelWorld_.isChunkLoaded(chunkPos)) {
        return false;
    }
    return actorWorld_.loadEntitiesInChunk(chunkPos) && voxelWorld_.loadChunk(chunkPos);
}

bool World::unloadChunkServer(glm::ivec3 chunkPos) {
    if (!voxelWorld_.isChunkLoaded(chunkPos)) {
        return false;
    }
    return actorWorld_.unloadEntitiesInChunk(chunkPos) && voxelWorld_.unloadChunk(chunkPos);
}

bool World::loadChunkClient(glm::ivec3 chunkPos) {
    return voxelWorld_.loadChunk(chunkPos);
}

bool World::unloadChunkClient(glm::ivec3 chunkPos) {
    return voxelWorld_.unloadChunk(chunkPos);
}

BlockData World::getBlockClient(glm::ivec3 worldPos) const {
    return voxelWorld_.getBlockOrAir(worldPos);
}

void World::setBlockServer(glm::ivec3 worldPos, BlockData blockData) {
    voxelWorld_.setBlock(worldPos, blockData);
}

void World::setBlockClientFromSnapshot(glm::ivec3 worldPos, BlockData blockData) {
    const glm::ivec3 chunkPos = worldPos / Chunk::SIZE;
    if (!voxelWorld_.isChunkLoaded(chunkPos)) {
        voxelWorld_.loadChunk(chunkPos);
    }
    voxelWorld_.setBlockIfChunkLoaded(worldPos, blockData);
}

std::vector<glm::ivec3> World::getLoadedChunks() const {
    return voxelWorld_.getLoadedChunks();
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
