#pragma once

#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <memory>
#include <unordered_map>

#include "block.h"
#include "chunk.h"

class VoxelWorld {
public:
    Chunk& getChunk(glm::ivec3 chunkPos);
    bool isChunkLoaded(glm::ivec3 chunkPos) const;

    bool loadChunk(glm::ivec3 chunkPos);
    bool unloadChunk(glm::ivec3 chunkPos);

    BlockData getBlock(glm::ivec3 worldPos) const;
    void setBlock(glm::ivec3 worldPos, BlockData blockData);

private:
    std::unordered_map<glm::ivec3, std::unique_ptr<Chunk>> chunks_;
};
