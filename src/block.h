#pragma once

#include <cstdint>

enum class BlockType : uint8_t {
    Air,
    Stone,
    Dirt,
    Grass,
    Wood,
    Leaves,
    Water,
    Sand,
    Count,
};

enum class BlockOrientation : uint8_t {
    North,
    East,
    South,
    West,
    Up,
    Down,
    Count,
};

struct BlockData {
    BlockType type;
    BlockOrientation orientation;

    constexpr BlockData(BlockType t = BlockType::Air, BlockOrientation o = BlockOrientation::North)
        : type(t), orientation(o) {}

    constexpr bool operator==(const BlockData&) const = default;
};

enum class BlockQueryResult : uint8_t {
    Unknown,
    Empty,
    Solid,
};

constexpr uint16_t packBlock(BlockData block) {
    return static_cast<uint16_t>(static_cast<uint16_t>(block.type) << 8 | static_cast<uint16_t>(block.orientation));
}

constexpr BlockData unpackBlock(uint16_t packed) {
    return BlockData{static_cast<BlockType>(packed >> 8), static_cast<BlockOrientation>(packed & 0xFF)};
}

constexpr bool isValidBlock(BlockData block) {
    return block.type < BlockType::Count && block.orientation < BlockOrientation::Count;
}
