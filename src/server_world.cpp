#include "server_world.h"

#include <cstdint>
#include <cstdlib>

#include "profiler.h"

namespace {

constexpr glm::ivec3 kWorldMin{-1024, -256, -1024};
constexpr glm::ivec3 kWorldMax{1024, 256, 1024};
constexpr int kBaseTerrainHeight = 0;
constexpr int kTerrainHeightRange = 7;
constexpr int kDirtDepth = 3;
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

int interpolatedNoise(int x, int z, int step, int amplitude) {
    const int cellX = x >= 0 ? x / step : (x - step + 1) / step;
    const int cellZ = z >= 0 ? z / step : (z - step + 1) / step;
    const int localX = x - cellX * step;
    const int localZ = z - cellZ * step;

    const int h00 = static_cast<int>(hash2D(cellX, cellZ) % (amplitude * 2 + 1)) - amplitude;
    const int h10 = static_cast<int>(hash2D(cellX + 1, cellZ) % (amplitude * 2 + 1)) - amplitude;
    const int h01 = static_cast<int>(hash2D(cellX, cellZ + 1) % (amplitude * 2 + 1)) - amplitude;
    const int h11 = static_cast<int>(hash2D(cellX + 1, cellZ + 1) % (amplitude * 2 + 1)) - amplitude;

    const int hx0 = h00 + (h10 - h00) * localX / step;
    const int hx1 = h01 + (h11 - h01) * localX / step;
    return hx0 + (hx1 - hx0) * localZ / step;
}

int terrainHeight(int x, int z) {
    int height = kBaseTerrainHeight;
    height += interpolatedNoise(x, z, 64, 6);
    height += interpolatedNoise(x + 113, z - 71, 24, 4);
    height += interpolatedNoise(x - 29, z + 47, 8, 2);
    return glm::clamp(height, -kTerrainHeightRange, kTerrainHeightRange);
}

bool shouldPlaceTree(glm::ivec3 worldPos) {
    return (hash2D(worldPos.x, worldPos.z) & kTreeChanceMask) == kTreeChanceValue;
}

int treeHeight(glm::ivec3 worldPos) {
    return kTreeMinHeight + static_cast<int>((hash2D(worldPos.x + 17, worldPos.z - 31) >> 8) % kTreeHeightRange);
}

bool generateTreeBlock(glm::ivec3 worldPos, BlockData& blockData) {
    for (int baseX = worldPos.x - 2; baseX <= worldPos.x + 2; ++baseX) {
        for (int baseZ = worldPos.z - 2; baseZ <= worldPos.z + 2; ++baseZ) {
            const int surfaceY = terrainHeight(baseX, baseZ);
            const glm::ivec3 basePos(baseX, surfaceY, baseZ);
            if (!shouldPlaceTree(basePos)) {
                continue;
            }

            const int height = treeHeight(basePos);
            if (worldPos.x == baseX && worldPos.z == baseZ &&
                worldPos.y > surfaceY && worldPos.y <= surfaceY + height) {
                blockData = BlockData{BlockType::Wood, BlockOrientation::Up};
                return true;
            }

            const glm::ivec3 leafCenter = basePos + glm::ivec3(0, height + 1, 0);
            const glm::ivec3 leafOffset = worldPos - leafCenter;
            const int distance = std::abs(leafOffset.x) + std::abs(leafOffset.y) + std::abs(leafOffset.z);
            if (distance > 3) {
                continue;
            }
            if (leafOffset.x == 0 && leafOffset.z == 0 && leafOffset.y <= 0) {
                continue;
            }

            blockData = BlockData{BlockType::Leaves, BlockOrientation::North};
            return true;
        }
    }

    return false;
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
}

BlockData ServerWorld::generateBlock(glm::ivec3 worldPos) const {
    if (!isBlockInBounds(worldPos)) {
        return BlockData{BlockType::Air, BlockOrientation::North};
    }

    BlockData treeBlock;
    if (generateTreeBlock(worldPos, treeBlock)) {
        return treeBlock;
    }

    const int surfaceY = terrainHeight(worldPos.x, worldPos.z);
    if (worldPos.y > surfaceY) {
        return BlockData{BlockType::Air, BlockOrientation::North};
    }
    if (worldPos.y == surfaceY) {
        return BlockData{BlockType::Grass, BlockOrientation::North};
    }
    if (worldPos.y >= surfaceY - kDirtDepth) {
        return BlockData{BlockType::Dirt, BlockOrientation::North};
    }
    return BlockData{BlockType::Stone, BlockOrientation::North};
}

bool ServerWorld::isBlockInBounds(glm::ivec3 worldPos) const {
    return worldPos.x >= kWorldMin.x && worldPos.x < kWorldMax.x &&
           worldPos.y >= kWorldMin.y && worldPos.y < kWorldMax.y &&
           worldPos.z >= kWorldMin.z && worldPos.z < kWorldMax.z;
}
