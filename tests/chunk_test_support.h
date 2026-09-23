#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <random>

#include "chunk_data.h"

namespace test_support {

struct Rng {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<uint32_t>(state >> 32);
    }
    uint32_t below(uint32_t bound) { return next() % bound; }
};

inline ChunkData patternedData(bool noise) {
    ChunkData data;
    std::mt19937 random(42);
    for (size_t i = 0; i < ChunkLayout::kBlockCount; ++i) {
        const auto type = noise ? random() % 8 : (i / 256) % 3;
        const auto orientation = noise ? random() % 6 : 0;
        data.set(i, BlockData{static_cast<BlockType>(type), static_cast<BlockOrientation>(orientation)});
    }
    return data;
}

inline void sameBlocks(const ChunkData& a, const ChunkData& b) {
    for (size_t i = 0; i < ChunkLayout::kBlockCount; ++i) {
        ASSERT_EQ(a.get(i), b.get(i)) << "Block index: " << i;
    }
}

}  // namespace test_support
