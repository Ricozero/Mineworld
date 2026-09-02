#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

#include "block.h"
#include "chunk_data.h"
#include "chunk_layout.h"

class Chunk {
public:
    explicit Chunk(glm::ivec3 chunkPos) : chunkPos_(chunkPos) {}
    ~Chunk() = default;

    glm::ivec3 localToWorld(glm::ivec3 localPos) const { return ChunkLayout::localToWorld(chunkPos_, localPos); }

    glm::ivec3 getPosition() const { return chunkPos_; }
    uint32_t getRevision() const { return revision_; }
    const ChunkData& getData() const { return data_; }
    BlockData getBlock(size_t index) const { return data_.get(index); }
    BlockData getBlock(glm::ivec3 localPos) const;
    void setBlock(glm::ivec3 localPos, BlockData blockData);
    void clearBlock(glm::ivec3 localPos);
    bool applyData(uint32_t revision, ChunkData&& data);

    bool isUniform() const { return data_.isUniform(); }
    BlockData uniformBlock() const { return data_.uniformBlock(); }
    size_t blockCount() const { return data_.blockCount(); }
    bool isEmpty() const { return blockCount() == 0; }

private:
    glm::ivec3 chunkPos_{0};
    uint32_t revision_ = 0;
    ChunkData data_;
};
