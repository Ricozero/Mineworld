#pragma once

#include <cstdint>

enum class BlockType : uint8_t {
    Air = 0,
    Stone = 1,
    Dirt = 2,
    Grass = 3,
    Wood = 4,
    Leaves = 5,
    Water = 6,
    Sand = 7
};

// 添加方块朝向枚举
enum class BlockOrientation : uint8_t {
    North = 0,
    East = 1,
    South = 2,
    West = 3,
    Up = 4,
    Down = 5
};

struct BlockData {
    BlockType type;
    BlockOrientation orientation;

    BlockData(BlockType t = BlockType::Air, BlockOrientation o = BlockOrientation::North)
        : type(t), orientation(o) {}
};