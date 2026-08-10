#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

#include "block.h"

struct ChunkData {
    static constexpr int SIZE = 16;
    static constexpr size_t BLOCK_COUNT = SIZE * SIZE * SIZE;

    glm::ivec3 chunkPos{0};
    uint32_t revision = 0;
    std::array<BlockData, BLOCK_COUNT> blocks{};

    static size_t blockIndex(glm::ivec3 localPos);
};

class Chunk {
public:
    Chunk(glm::ivec3 chunkPos);
    ~Chunk() = default;

    static bool isValidLocalPosition(glm::ivec3 pos);
    static glm::ivec3 worldToChunk(glm::ivec3 worldPos);
    static glm::ivec3 worldToLocal(glm::ivec3 worldPos);
    glm::ivec3 localToWorld(glm::ivec3 localPos) const;

    glm::ivec3 getPosition() const { return data_.chunkPos; }
    uint32_t getRevision() const { return data_.revision; }
    BlockData getBlock(glm::ivec3 localPos) const;
    void setBlock(glm::ivec3 localPos, BlockData blockData);
    void clearBlock(glm::ivec3 localPos);
    bool applyData(const ChunkData& data);
    ChunkData buildData() const;

    size_t getBlockCount() const;
    bool isEmpty() const { return getBlockCount() == 0; }

private:
    ChunkData data_;
};
