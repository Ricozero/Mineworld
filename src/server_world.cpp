#include "server_world.h"

#include <cstdint>
#include <cstdlib>

#include "profiler.h"

namespace {

constexpr glm::ivec3 kWorldMin{-1024, -256, -1024};
constexpr glm::ivec3 kWorldMax{1024, 256, 1024};
constexpr int kTreeSurfaceY = 0;
constexpr int kTreeMinHeight = 4;
constexpr int kTreeHeightRange = 3;
constexpr uint32_t kTreeChanceMask = 0x7f;
constexpr uint32_t kTreeChanceValue = 0;

uint32_t hash2D(int x, int z) {
    uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u;
    h ^= static_cast<uint32_t>(z) * 0xd8163841u;
    h ^= h >> 13;
    h *= 0xcb1ab31fu;
    h ^= h >> 16;
    return h;
}

bool shouldPlaceTree(glm::ivec3 worldPos) {
    return (hash2D(worldPos.x, worldPos.z) & kTreeChanceMask) == kTreeChanceValue;
}

int treeHeight(glm::ivec3 worldPos) {
    return kTreeMinHeight + static_cast<int>((hash2D(worldPos.x + 17, worldPos.z - 31) >> 8) % kTreeHeightRange);
}

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

void ServerWorld::generateChunk(Chunk& chunk) const {
    MW_PROFILE_SCOPE("Server.GenerateChunk");
    MW_PROFILE_COUNTER("Server.ChunksGenerated", 1);

    for (int x = 0; x < Chunk::SIZE; ++x) {
        for (int y = 0; y < Chunk::SIZE; ++y) {
            for (int z = 0; z < Chunk::SIZE; ++z) {
                const glm::ivec3 localPos(x, y, z);
                chunk.setBlock(localPos, generateBlock(chunk.localToWorld(localPos)));
            }
        }
    }

    const glm::ivec3 chunkPos = chunk.getPosition();
    for (int x = 1; x < Chunk::SIZE - 1; ++x) {
        for (int z = 1; z < Chunk::SIZE - 1; ++z) {
            const glm::ivec3 surfaceLocalPos(x, kTreeSurfaceY - chunkPos.y * Chunk::SIZE, z);
            if (!Chunk::isValidLocalPosition(surfaceLocalPos)) {
                continue;
            }

            const glm::ivec3 surfaceWorldPos = chunk.localToWorld(surfaceLocalPos);
            if (!shouldPlaceTree(surfaceWorldPos)) {
                continue;
            }

            const int height = treeHeight(surfaceWorldPos);
            for (int i = 1; i <= height; ++i) {
                const glm::ivec3 localPos = surfaceLocalPos + glm::ivec3(0, i, 0);
                if (Chunk::isValidLocalPosition(localPos)) {
                    chunk.setBlock(localPos, BlockData{BlockType::Wood, BlockOrientation::Up});
                }
            }

            const glm::ivec3 leafCenter = surfaceLocalPos + glm::ivec3(0, height + 1, 0);
            for (int dx = -2; dx <= 2; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dz = -2; dz <= 2; ++dz) {
                        const int distance = std::abs(dx) + std::abs(dy) + std::abs(dz);
                        if (distance > 3) {
                            continue;
                        }
                        if (dx == 0 && dz == 0 && dy <= 0) {
                            continue;
                        }
                        const glm::ivec3 localPos = leafCenter + glm::ivec3(dx, dy, dz);
                        if (Chunk::isValidLocalPosition(localPos)) {
                            chunk.setBlock(localPos, BlockData{BlockType::Leaves, BlockOrientation::North});
                        }
                    }
                }
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
