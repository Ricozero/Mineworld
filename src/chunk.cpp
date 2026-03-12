#include "chunk.h"

#include <spdlog/spdlog.h>

#include <glm/gtx/string_cast.hpp>

Chunk::Chunk(glm::ivec3 chunkPos) : chunkPosition_(chunkPos) {
    for (int x = 0; x < SIZE; ++x) {
        for (int y = 0; y < SIZE; ++y) {
            for (int z = 0; z < SIZE; ++z) {
                blocks_[x][y][z] = BlockData{BlockType::Air, BlockOrientation::North};
            }
        }
    }
}

bool Chunk::isValidLocalPosition(glm::ivec3 pos) {
    return pos.x >= 0 && pos.x < SIZE && pos.y >= 0 && pos.y < SIZE && pos.z >= 0 && pos.z < SIZE;
}

glm::ivec3 Chunk::worldToLocal(glm::ivec3 worldPos) {
    return worldPos - worldPos / SIZE * SIZE;
}

glm::ivec3 Chunk::localToWorld(glm::ivec3 localPos) const {
    return glm::ivec3(chunkPosition_.x * SIZE + localPos.x,
                      chunkPosition_.y * SIZE + localPos.y,
                      chunkPosition_.z * SIZE + localPos.z);
}

BlockData Chunk::getBlock(glm::ivec3 localPos) const {
    if (isValidLocalPosition(localPos)) {
        return blocks_[localPos.x][localPos.y][localPos.z];
    } else {
        spdlog::error("Attempted to get block at {}", glm::to_string(localToWorld(localPos)));
        return BlockData{BlockType::Air, BlockOrientation::North};
    }
}

void Chunk::setBlock(glm::ivec3 localPos, BlockData blockData) {
    if (isValidLocalPosition(localPos)) {
        blocks_[localPos.x][localPos.y][localPos.z] = blockData;
    } else {
        spdlog::error("Attempted to set block at {}", glm::to_string(localToWorld(localPos)));
    }
}

void Chunk::clearBlock(glm::ivec3 localPos) {
    if (isValidLocalPosition(localPos)) {
        blocks_[localPos.x][localPos.y][localPos.z] = BlockData{BlockType::Air, BlockOrientation::North};
    } else {
        spdlog::error("Attempted to clear block at {}", glm::to_string(localToWorld(localPos)));
    }
}

size_t Chunk::getBlockCount() const {
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