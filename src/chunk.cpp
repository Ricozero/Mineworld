#include "chunk.h"

#include <glm/gtx/string_cast.hpp>

#include "log.h"

size_t ChunkData::blockIndex(glm::ivec3 localPos) {
    return (static_cast<size_t>(localPos.x) * SIZE + static_cast<size_t>(localPos.y)) * SIZE + static_cast<size_t>(localPos.z);
}

Chunk::Chunk(glm::ivec3 chunkPos) {
    data_.chunkPos = chunkPos;
}

bool Chunk::isValidLocalPosition(glm::ivec3 pos) {
    return pos.x >= 0 && pos.x < ChunkData::SIZE &&
           pos.y >= 0 && pos.y < ChunkData::SIZE &&
           pos.z >= 0 && pos.z < ChunkData::SIZE;
}

glm::ivec3 Chunk::worldToChunk(glm::ivec3 worldPos) {
    auto floorDiv = [](int value) {
        return value >= 0 ? value / ChunkData::SIZE : (value - ChunkData::SIZE + 1) / ChunkData::SIZE;
    };
    return glm::ivec3(floorDiv(worldPos.x), floorDiv(worldPos.y), floorDiv(worldPos.z));
}

glm::ivec3 Chunk::worldToLocal(glm::ivec3 worldPos) {
    const glm::ivec3 chunkPos = worldToChunk(worldPos);
    return worldPos - chunkPos * ChunkData::SIZE;
}

glm::ivec3 Chunk::localToWorld(glm::ivec3 localPos) const {
    return glm::ivec3(data_.chunkPos.x * ChunkData::SIZE + localPos.x,
                      data_.chunkPos.y * ChunkData::SIZE + localPos.y,
                      data_.chunkPos.z * ChunkData::SIZE + localPos.z);
}

BlockData Chunk::getBlock(glm::ivec3 localPos) const {
    if (isValidLocalPosition(localPos)) {
        return data_.blocks[ChunkData::blockIndex(localPos)];
    } else {
        return BlockData{BlockType::Air, BlockOrientation::North};
    }
}

void Chunk::setBlock(glm::ivec3 localPos, BlockData blockData) {
    if (isValidLocalPosition(localPos)) {
        BlockData& current = data_.blocks[ChunkData::blockIndex(localPos)];
        if (current.type != blockData.type || current.orientation != blockData.orientation) {
            current = blockData;
            ++data_.revision;
        }
    } else {
        logging::error("Attempted to set block at {}", glm::to_string(localToWorld(localPos)));
    }
}

void Chunk::clearBlock(glm::ivec3 localPos) {
    if (isValidLocalPosition(localPos)) {
        setBlock(localPos, BlockData{BlockType::Air, BlockOrientation::North});
    } else {
        logging::error("Attempted to clear block at {}", glm::to_string(localToWorld(localPos)));
    }
}

bool Chunk::applyData(const ChunkData& data) {
    if (data.chunkPos != data_.chunkPos || data.revision < data_.revision) {
        return false;
    }
    data_ = data;
    return true;
}

ChunkData Chunk::buildData() const {
    return data_;
}

size_t Chunk::getBlockCount() const {
    size_t count = 0;
    for (const BlockData& block : data_.blocks) {
        if (block.type != BlockType::Air) {
            ++count;
        }
    }
    return count;
}
