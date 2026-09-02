#pragma once

#include <cstddef>
#include <glm/glm.hpp>

struct ChunkLayout {
    static constexpr int SIZE = 16;
    static constexpr int SIZE_BITS = 4;
    static constexpr size_t BLOCK_COUNT = size_t{SIZE} * SIZE * SIZE;
    static constexpr int X_STRIDE = 1;
    static constexpr int Z_STRIDE = SIZE;
    static constexpr int Y_STRIDE = SIZE * SIZE;

    static constexpr glm::ivec3 WORLD_MIN{-1024, -256, -1024};
    static constexpr glm::ivec3 WORLD_MAX{1024, 256, 1024};

    static constexpr size_t blockIndex(glm::ivec3 localPos) {
        return (static_cast<size_t>(localPos.y) << (SIZE_BITS * 2)) |
               (static_cast<size_t>(localPos.z) << SIZE_BITS) |
               static_cast<size_t>(localPos.x);
    }

    static constexpr glm::ivec3 blockPosition(size_t index) {
        return glm::ivec3(static_cast<int>(index & (SIZE - 1)),
                          static_cast<int>(index >> (SIZE_BITS * 2)),
                          static_cast<int>((index >> SIZE_BITS) & (SIZE - 1)));
    }

    static constexpr bool isValidLocalPosition(glm::ivec3 pos) {
        return pos.x >= 0 && pos.x < SIZE &&
               pos.y >= 0 && pos.y < SIZE &&
               pos.z >= 0 && pos.z < SIZE;
    }

    static constexpr glm::ivec3 worldToChunk(glm::ivec3 worldPos) {
        return glm::ivec3(worldPos.x >> SIZE_BITS, worldPos.y >> SIZE_BITS, worldPos.z >> SIZE_BITS);
    }

    static constexpr bool isChunkInWorld(glm::ivec3 chunkPos) {
        const glm::ivec3 minChunk = worldToChunk(WORLD_MIN);
        const glm::ivec3 maxChunk = worldToChunk(WORLD_MAX);
        return chunkPos.x >= minChunk.x && chunkPos.x < maxChunk.x &&
               chunkPos.y >= minChunk.y && chunkPos.y < maxChunk.y &&
               chunkPos.z >= minChunk.z && chunkPos.z < maxChunk.z;
    }

    static constexpr glm::ivec3 worldToLocal(glm::ivec3 worldPos) {
        return glm::ivec3(worldPos.x & (SIZE - 1), worldPos.y & (SIZE - 1), worldPos.z & (SIZE - 1));
    }

    static constexpr glm::ivec3 localToWorld(glm::ivec3 chunkPos, glm::ivec3 localPos) {
        return glm::ivec3(chunkPos.x * SIZE + localPos.x,
                          chunkPos.y * SIZE + localPos.y,
                          chunkPos.z * SIZE + localPos.z);
    }
};

static_assert(ChunkLayout::SIZE == 1 << ChunkLayout::SIZE_BITS);
static_assert(ChunkLayout::blockIndex({0, 0, 0}) == 0);
static_assert(ChunkLayout::blockIndex({15, 15, 15}) == ChunkLayout::BLOCK_COUNT - 1);
static_assert(ChunkLayout::blockIndex({1, 2, 3}) == ((size_t{2} << 8) | (size_t{3} << 4) | size_t{1}));
static_assert(ChunkLayout::blockIndex({2, 1, 1}) - ChunkLayout::blockIndex({1, 1, 1}) == size_t{ChunkLayout::X_STRIDE});
static_assert(ChunkLayout::blockIndex({1, 1, 2}) - ChunkLayout::blockIndex({1, 1, 1}) == size_t{ChunkLayout::Z_STRIDE});
static_assert(ChunkLayout::blockIndex({1, 2, 1}) - ChunkLayout::blockIndex({1, 1, 1}) == size_t{ChunkLayout::Y_STRIDE});
static_assert(ChunkLayout::worldToChunk({-1, 0, 16}).x == -1 && ChunkLayout::worldToChunk({-1, 0, 16}).z == 1);
static_assert(ChunkLayout::worldToChunk({-16, -17, 15}).x == -1 && ChunkLayout::worldToChunk({-16, -17, 15}).y == -2);
static_assert(ChunkLayout::worldToLocal({-1, 0, 16}).x == 15 && ChunkLayout::worldToLocal({-1, 0, 16}).z == 0);
static_assert(ChunkLayout::worldToLocal({-16, -17, 15}).x == 0 && ChunkLayout::worldToLocal({-16, -17, 15}).y == 15);
static_assert(ChunkLayout::WORLD_MIN.x % ChunkLayout::SIZE == 0 && ChunkLayout::WORLD_MIN.y % ChunkLayout::SIZE == 0 &&
                  ChunkLayout::WORLD_MIN.z % ChunkLayout::SIZE == 0 && ChunkLayout::WORLD_MAX.x % ChunkLayout::SIZE == 0 &&
                  ChunkLayout::WORLD_MAX.y % ChunkLayout::SIZE == 0 && ChunkLayout::WORLD_MAX.z % ChunkLayout::SIZE == 0,
              "World bounds must be chunk aligned so that a chunk is either wholly in or wholly out of bounds");
static_assert(ChunkLayout::isChunkInWorld({0, 0, 0}));
static_assert(ChunkLayout::isChunkInWorld({-64, -16, -64}));
static_assert(!ChunkLayout::isChunkInWorld({64, 0, 0}));
static_assert(!ChunkLayout::isChunkInWorld({0, -17, 0}));

constexpr bool blockIndexRoundTrips() {
    for (size_t index = 0; index < ChunkLayout::BLOCK_COUNT; ++index) {
        if (ChunkLayout::blockIndex(ChunkLayout::blockPosition(index)) != index) {
            return false;
        }
    }
    return true;
}
static_assert(blockIndexRoundTrips(), "blockIndex and blockPosition must be inverses");
