#pragma once

#include <glm/glm.hpp>

#include "block.h"

class Chunk {
public:
    static constexpr int SIZE = 16;

    Chunk(glm::ivec3 chunkPos);
    ~Chunk() = default;

    static bool isValidLocalPosition(glm::ivec3 pos);
    static glm::ivec3 worldToLocal(glm::ivec3 worldPos);
    glm::ivec3 localToWorld(glm::ivec3 localPos) const;

    glm::ivec3 getPosition() const { return chunkPosition_; }
    BlockData getBlock(glm::ivec3 localPos) const;
    void setBlock(glm::ivec3 localPos, BlockData blockData);
    void clearBlock(glm::ivec3 localPos);

    size_t getFullBlockCount() const { return SIZE * SIZE * SIZE; }
    size_t getBlockCount() const;
    bool isEmpty() const { return getBlockCount() == 0; }

private:
    glm::ivec3 chunkPosition_;
    BlockData blocks_[SIZE][SIZE][SIZE];
};
