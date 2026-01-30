#pragma once

#include <glm/glm.hpp>

#include "block.h"

// 区块（Chunk）类
// 区块是Minecraft式游戏中的基本存储单位，大小为16x16x16
// 每个区块包含该范围内的所有方块实体的引用

class Chunk {
public:
    static constexpr int SIZE = 16;  // 区块边长

    // 构造函数：创建一个指定世界坐标的空区块
    Chunk(glm::ivec3 chunkPos);
    ~Chunk() = default;

    // ========== 访问器 ==========
    // 获取区块位置
    glm::ivec3 getPosition() const { return chunkPosition_; }

    // ========== 方块操作 ==========
    // 获取指定本地坐标的方块数据
    BlockData getBlock(int localX, int localY, int localZ) const;

    // 设置指定本地坐标的方块数据
    void setBlock(int localX, int localY, int localZ, BlockData blockData);

    // 清除指定本地坐标的方块
    void clearBlock(int localX, int localY, int localZ);

    // ========== 验证 ==========
    // 检查本地坐标是否有效
    static bool isValidLocalPosition(int x, int y, int z);

    // 将世界坐标转换为本地坐标
    static glm::ivec3 worldToLocal(glm::ivec3 worldPos, glm::ivec3 chunkPos);

    // 将本地坐标转换为世界坐标
    glm::ivec3 localToWorld(int localX, int localY, int localZ) const;

    // ========== 统计 ==========
    // 获取区块中非空方块的数量
    size_t getNonEmptyBlockCount() const;

    // 获取区块中所有方块的数量
    size_t getTotalBlockCount() const { return SIZE * SIZE * SIZE; }

    // 检查区块是否为空
    bool isEmpty() const { return getNonEmptyBlockCount() == 0; }

private:
    glm::ivec3 chunkPosition_;  // 区块在世界中的位置（区块坐标）

    // 3D数组存储区块内的方块数据
    // blocks_[x][y][z] = 方块类型和朝向数据
    BlockData blocks_[SIZE][SIZE][SIZE];
};
