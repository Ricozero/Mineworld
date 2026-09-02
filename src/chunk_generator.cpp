#include "chunk_generator.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>

#include "chunk_layout.h"
#include "profiler.h"

namespace {

constexpr int kBaseHeight = 0;
constexpr int kDirtDepth = 3;
constexpr int kTerrainAmplitude = 4;
constexpr float kTerrainFrequency = 0.045f;
constexpr int kTreeMinHeight = 4;
constexpr int kTreeHeightVariation = 3;
constexpr int kTreeLeafRadius = 3;
constexpr int kTreeColumnRadius = 2;
constexpr int kMaxTreeTop = kTreeMinHeight + kTreeHeightVariation - 1 + kTreeLeafRadius;
constexpr int kHeightFieldSize = ChunkLayout::SIZE + 2 * kTreeColumnRadius;

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

struct HeightField {
    glm::ivec2 origin{0};
    int minHeight = 0;
    int maxHeight = 0;
    std::array<int, kHeightFieldSize * kHeightFieldSize> heights{};

    int at(int worldX, int worldZ) const {
        const int x = worldX - origin.x;
        const int z = worldZ - origin.y;
        assert(x >= 0 && x < kHeightFieldSize && z >= 0 && z < kHeightFieldSize);
        return heights[static_cast<size_t>(z) * kHeightFieldSize + x];
    }
};

HeightField buildHeightField(glm::ivec3 chunkMin) {
    HeightField field;
    field.origin = glm::ivec2(chunkMin.x - kTreeColumnRadius, chunkMin.z - kTreeColumnRadius);
    field.minHeight = std::numeric_limits<int>::max();
    field.maxHeight = std::numeric_limits<int>::min();

    for (int z = 0; z < kHeightFieldSize; ++z) {
        for (int x = 0; x < kHeightFieldSize; ++x) {
            const int height = terrainHeight(field.origin.x + x, field.origin.y + z);
            field.heights[static_cast<size_t>(z) * kHeightFieldSize + x] = height;
            field.maxHeight = std::max(field.maxHeight, height);
            if (x >= kTreeColumnRadius && x < kTreeColumnRadius + ChunkLayout::SIZE &&
                z >= kTreeColumnRadius && z < kTreeColumnRadius + ChunkLayout::SIZE) {
                field.minHeight = std::min(field.minHeight, height);
            }
        }
    }
    return field;
}

bool shouldPlaceTree(glm::ivec3 worldPos) {
    return hash2D(worldPos.x, worldPos.z) % 199u == 0u;
}

int treeHeight(glm::ivec3 worldPos) {
    return kTreeMinHeight + static_cast<int>(hash2D(worldPos.x + 17, worldPos.z - 11) % kTreeHeightVariation);
}

bool generateTreeBlock(glm::ivec3 worldPos, const HeightField& field, BlockData& blockData) {
    for (int rootX = worldPos.x - kTreeColumnRadius; rootX <= worldPos.x + kTreeColumnRadius; ++rootX) {
        for (int rootZ = worldPos.z - kTreeColumnRadius; rootZ <= worldPos.z + kTreeColumnRadius; ++rootZ) {
            const glm::ivec3 rootPos(rootX, field.at(rootX, rootZ), rootZ);
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
            if (distance > kTreeLeafRadius) {
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

BlockData generateBlock(glm::ivec3 worldPos, const HeightField& field) {
    BlockData treeBlock;
    if (generateTreeBlock(worldPos, field, treeBlock)) {
        return treeBlock;
    }

    const int surfaceY = field.at(worldPos.x, worldPos.z);
    if (worldPos.y > surfaceY) {
        return BlockData{};
    }
    if (worldPos.y == surfaceY) {
        return BlockData{BlockType::Grass, BlockOrientation::North};
    }
    if (worldPos.y >= surfaceY - kDirtDepth) {
        return BlockData{BlockType::Dirt, BlockOrientation::North};
    }
    return BlockData{BlockType::Stone, BlockOrientation::North};
}

}  // namespace

ChunkData ChunkGenerator::generate(glm::ivec3 chunkPos) {
    MW_PROFILE_COUNTER("Server.ChunksGenerated", 1);

    ChunkData data;

    if (!ChunkLayout::isChunkInWorld(chunkPos)) {
        data.fill(BlockData{});
        return data;
    }

    const glm::ivec3 chunkMin = chunkPos * ChunkLayout::SIZE;
    const int chunkMaxY = chunkMin.y + ChunkLayout::SIZE - 1;
    const HeightField field = buildHeightField(chunkMin);

    if (chunkMin.y > field.maxHeight + kMaxTreeTop) {
        data.fill(BlockData{});
        return data;
    }
    if (chunkMaxY < field.minHeight - kDirtDepth) {
        data.fill(BlockData{BlockType::Stone, BlockOrientation::North});
        return data;
    }

    for (size_t index = 0; index < ChunkLayout::BLOCK_COUNT; ++index) {
        const glm::ivec3 worldPos = chunkMin + ChunkLayout::blockPosition(index);
        data.set(index, generateBlock(worldPos, field));
    }
    data.optimize();
    return data;
}
