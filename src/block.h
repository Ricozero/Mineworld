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
    Sand
};

enum class BlockOrientation : uint8_t {
    North,
    East,
    South,
    West,
    Up,
    Down
};

struct BlockData {
    BlockType type;
    BlockOrientation orientation;

    BlockData(BlockType t = BlockType::Air, BlockOrientation o = BlockOrientation::North)
        : type(t), orientation(o) {}
};