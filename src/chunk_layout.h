#pragma once

#include <cstddef>
#include <glm/glm.hpp>

struct ChunkLayout {
    static constexpr int kSize = 16;
    static constexpr int kSizeBits = 4;
    static constexpr size_t kBlockCount = size_t{kSize} * kSize * kSize;
    static constexpr int kXStride = 1;
    static constexpr int kZStride = kSize;
    static constexpr int kYStride = kSize * kSize;

    static constexpr glm::ivec3 kWorldMin{-65536, -256, -65536};
    static constexpr glm::ivec3 kWorldMax{65536, 256, 65536};

    static constexpr size_t blockIndex(glm::ivec3 localPos) {
        return (static_cast<size_t>(localPos.y) << (kSizeBits * 2)) |
               (static_cast<size_t>(localPos.z) << kSizeBits) |
               static_cast<size_t>(localPos.x);
    }

    static constexpr glm::ivec3 blockPosition(size_t index) {
        return glm::ivec3(static_cast<int>(index & (kSize - 1)),
                          static_cast<int>(index >> (kSizeBits * 2)),
                          static_cast<int>((index >> kSizeBits) & (kSize - 1)));
    }

    static constexpr bool isValidLocalPosition(glm::ivec3 pos) {
        return pos.x >= 0 && pos.x < kSize &&
               pos.y >= 0 && pos.y < kSize &&
               pos.z >= 0 && pos.z < kSize;
    }

    static constexpr glm::ivec3 blockToChunk(glm::ivec3 blockPos) {
        return glm::ivec3(blockPos.x >> kSizeBits, blockPos.y >> kSizeBits, blockPos.z >> kSizeBits);
    }

    static glm::ivec3 worldToBlock(glm::vec3 worldPos) {
        return glm::ivec3(glm::floor(worldPos));
    }

    static glm::ivec3 worldToChunk(glm::vec3 worldPos) {
        return blockToChunk(worldToBlock(worldPos));
    }

    static constexpr bool isChunkInWorld(glm::ivec3 chunkPos) {
        const glm::ivec3 minChunk = blockToChunk(kWorldMin);
        const glm::ivec3 maxChunk = blockToChunk(kWorldMax);
        return chunkPos.x >= minChunk.x && chunkPos.x < maxChunk.x &&
               chunkPos.y >= minChunk.y && chunkPos.y < maxChunk.y &&
               chunkPos.z >= minChunk.z && chunkPos.z < maxChunk.z;
    }

    static constexpr glm::ivec3 worldToLocal(glm::ivec3 worldPos) {
        return glm::ivec3(worldPos.x & (kSize - 1), worldPos.y & (kSize - 1), worldPos.z & (kSize - 1));
    }

    static constexpr glm::ivec3 localToWorld(glm::ivec3 chunkPos, glm::ivec3 localPos) {
        return glm::ivec3(chunkPos.x * kSize + localPos.x,
                          chunkPos.y * kSize + localPos.y,
                          chunkPos.z * kSize + localPos.z);
    }
};

static_assert(ChunkLayout::kSize == 1 << ChunkLayout::kSizeBits);
static_assert(ChunkLayout::blockIndex({0, 0, 0}) == 0);
static_assert(ChunkLayout::blockIndex({15, 15, 15}) == ChunkLayout::kBlockCount - 1);
static_assert(ChunkLayout::blockIndex({1, 2, 3}) == ((size_t{2} << 8) | (size_t{3} << 4) | size_t{1}));
static_assert(ChunkLayout::blockIndex({2, 1, 1}) - ChunkLayout::blockIndex({1, 1, 1}) == size_t{ChunkLayout::kXStride});
static_assert(ChunkLayout::blockIndex({1, 1, 2}) - ChunkLayout::blockIndex({1, 1, 1}) == size_t{ChunkLayout::kZStride});
static_assert(ChunkLayout::blockIndex({1, 2, 1}) - ChunkLayout::blockIndex({1, 1, 1}) == size_t{ChunkLayout::kYStride});
static_assert(ChunkLayout::blockToChunk({-1, 0, 16}).x == -1 && ChunkLayout::blockToChunk({-1, 0, 16}).z == 1);
static_assert(ChunkLayout::blockToChunk({-16, -17, 15}).x == -1 && ChunkLayout::blockToChunk({-16, -17, 15}).y == -2);
static_assert(ChunkLayout::worldToLocal({-1, 0, 16}).x == 15 && ChunkLayout::worldToLocal({-1, 0, 16}).z == 0);
static_assert(ChunkLayout::worldToLocal({-16, -17, 15}).x == 0 && ChunkLayout::worldToLocal({-16, -17, 15}).y == 15);
static_assert(ChunkLayout::kWorldMin.x % ChunkLayout::kSize == 0 && ChunkLayout::kWorldMin.y % ChunkLayout::kSize == 0 &&
                  ChunkLayout::kWorldMin.z % ChunkLayout::kSize == 0 && ChunkLayout::kWorldMax.x % ChunkLayout::kSize == 0 &&
                  ChunkLayout::kWorldMax.y % ChunkLayout::kSize == 0 && ChunkLayout::kWorldMax.z % ChunkLayout::kSize == 0,
              "World bounds must be chunk aligned so that a chunk is either wholly in or wholly out of bounds");
static_assert(ChunkLayout::isChunkInWorld({0, 0, 0}));
static_assert(ChunkLayout::isChunkInWorld({-64, -16, -64}));
static_assert(!ChunkLayout::isChunkInWorld({4096, 0, 0}));
static_assert(!ChunkLayout::isChunkInWorld({0, -17, 0}));

constexpr bool blockIndexRoundTrips() {
    for (size_t index = 0; index < ChunkLayout::kBlockCount; ++index) {
        if (ChunkLayout::blockIndex(ChunkLayout::blockPosition(index)) != index) {
            return false;
        }
    }
    return true;
}
static_assert(blockIndexRoundTrips(), "blockIndex and blockPosition must be inverses");
