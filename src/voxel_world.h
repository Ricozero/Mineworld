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
    const Chunk& getChunk(glm::ivec3 chunkPos) const;
    bool isChunkLoaded(glm::ivec3 chunkPos) const;

    bool loadChunk(const ChunkData& data);
    bool unloadChunk(glm::ivec3 chunkPos);
    ChunkData buildChunkData(glm::ivec3 chunkPos) const;

    BlockData getBlock(glm::ivec3 worldPos) const;
    BlockQueryResult queryBlock(glm::ivec3 worldPos) const;
    void setBlock(glm::ivec3 worldPos, BlockData blockData);

    template <typename Func>
    void forEachLoadedChunk(Func&& func) const {
        for (const auto& [chunkPos, _] : chunks_) {
            func(chunkPos);
        }
    }

private:
    std::unordered_map<glm::ivec3, std::unique_ptr<Chunk>> chunks_;
};
