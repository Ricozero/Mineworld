#include "chunk.h"

Chunk::Chunk(glm::ivec3 chunkPos) : chunkPosition_(chunkPos) {
    for (int x = 0; x < SIZE; ++x) {
        for (int y = 0; y < SIZE; ++y) {
            for (int z = 0; z < SIZE; ++z) {
                blocks_[x][y][z] = BlockData{BlockType::Air, BlockOrientation::North};
            }
        }
    }
}

BlockData Chunk::getBlock(int localX, int localY, int localZ) const {
    if (!isValidLocalPosition(localX, localY, localZ)) {
        return BlockData{BlockType::Air, BlockOrientation::North};
    }
    return blocks_[localX][localY][localZ];
}

void Chunk::setBlock(int localX, int localY, int localZ, BlockData blockData) {
    if (isValidLocalPosition(localX, localY, localZ)) {
        blocks_[localX][localY][localZ] = blockData;
    }
}

void Chunk::clearBlock(int localX, int localY, int localZ) {
    if (isValidLocalPosition(localX, localY, localZ)) {
        blocks_[localX][localY][localZ] = BlockData{BlockType::Air, BlockOrientation::North};
    }
}

bool Chunk::isValidLocalPosition(int x, int y, int z) {
    return x >= 0 && x < SIZE && y >= 0 && y < SIZE && z >= 0 && z < SIZE;
}

glm::ivec3 Chunk::worldToLocal(glm::ivec3 worldPos, glm::ivec3 chunkPos) {
    return worldPos - (chunkPos * SIZE);
}

glm::ivec3 Chunk::localToWorld(int localX, int localY, int localZ) const {
    return glm::ivec3(chunkPosition_.x * SIZE + localX,
                      chunkPosition_.y * SIZE + localY,
                      chunkPosition_.z * SIZE + localZ);
}

size_t Chunk::getNonEmptyBlockCount() const {
    size_t count = 0;
    for (int x = 0; x < SIZE; ++x) {
        for (int y = 0; y < SIZE; ++y) {
            for (int z = 0; z < SIZE; ++z) {
                if (blocks_[x][y][z].type != BlockType::Air) {
                    count++;
                }
            }
        }
    }
    return count;
}