#include "chunk_generator.h"

#include <cmath>
#include <cstdint>

#include "profiler.h"

namespace {

constexpr glm::ivec3 kWorldMin{-1024, -256, -1024};
constexpr glm::ivec3 kWorldMax{1024, 256, 1024};
constexpr int kBaseHeight = 0;
constexpr int kDirtDepth = 3;
constexpr int kTerrainAmplitude = 4;
constexpr float kTerrainFrequency = 0.045f;
constexpr int kTreeMinHeight = 4;
constexpr int kTreeHeightVariation = 3;

uint32_t hash2D(int x, int z) {
    uint32_t value = static_cast<uint32_t>(x) * 0x8da6b343u;
    value ^= static_cast<uint32_t>(z) * 0xd8163841u;
    value ^= value >> 13;
    value *= 0x85ebca6bu;
    value ^= value >> 16;
    return value;
}

int terrainHeight(int x, int z) {
    const float wave = std::sin(static_cast<float>(x) * kTerrainFrequency) +
                       std::cos(static_cast<float>(z) * kTerrainFrequency * 0.85f);
    return kBaseHeight + static_cast<int>(std::round(wave * static_cast<float>(kTerrainAmplitude)));
}

bool shouldPlaceTree(glm::ivec3 worldPos) {
    return hash2D(worldPos.x, worldPos.z) % 199u == 0u;
}

int treeHeight(glm::ivec3 worldPos) {
    return kTreeMinHeight + static_cast<int>(hash2D(worldPos.x + 17, worldPos.z - 11) % kTreeHeightVariation);
}

bool generateTreeBlock(glm::ivec3 worldPos, BlockData& blockData) {
    for (int rootX = worldPos.x - 2; rootX <= worldPos.x + 2; ++rootX) {
        for (int rootZ = worldPos.z - 2; rootZ <= worldPos.z + 2; ++rootZ) {
            const glm::ivec3 rootPos(rootX, terrainHeight(rootX, rootZ), rootZ);
            if (!shouldPlaceTree(rootPos)) {
                continue;
            }

            const int height = treeHeight(rootPos);
            if (worldPos.x == rootX && worldPos.z == rootZ && worldPos.y > rootPos.y && worldPos.y <= rootPos.y + height) {
                blockData = BlockData{BlockType::Wood, BlockOrientation::Up};
                return true;
            }

            const glm::ivec3 leafCenter(rootX, rootPos.y + height, rootZ);
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

ChunkData ChunkGenerator::generate(glm::ivec3 chunkPos) {
    MW_PROFILE_SCOPE("Server.GenerateChunk");
    MW_PROFILE_COUNTER("Server.ChunksGenerated", 1);

    ChunkData data;
    data.chunkPos = chunkPos;
    data.revision = 1;
    for (int x = 0; x < ChunkData::SIZE; ++x) {
        for (int y = 0; y < ChunkData::SIZE; ++y) {
            for (int z = 0; z < ChunkData::SIZE; ++z) {
                const glm::ivec3 localPos(x, y, z);
                const glm::ivec3 worldPos = chunkPos * ChunkData::SIZE + localPos;
                data.blocks[ChunkData::blockIndex(localPos)] = generateBlock(worldPos);
            }
        }
    }
    return data;
}

bool ChunkGenerator::isChunkInBounds(glm::ivec3 chunkPos) {
    const glm::ivec3 minChunk = Chunk::worldToChunk(kWorldMin);
    const glm::ivec3 maxChunk = Chunk::worldToChunk(kWorldMax);
    return chunkPos.x >= minChunk.x && chunkPos.x < maxChunk.x &&
           chunkPos.y >= minChunk.y && chunkPos.y < maxChunk.y &&
           chunkPos.z >= minChunk.z && chunkPos.z < maxChunk.z;
}

BlockData ChunkGenerator::generateBlock(glm::ivec3 worldPos) {
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

bool ChunkGenerator::isBlockInBounds(glm::ivec3 worldPos) {
    return worldPos.x >= kWorldMin.x && worldPos.x < kWorldMax.x &&
           worldPos.y >= kWorldMin.y && worldPos.y < kWorldMax.y &&
           worldPos.z >= kWorldMin.z && worldPos.z < kWorldMax.z;
}
