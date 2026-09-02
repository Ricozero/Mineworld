#pragma once

#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <memory>
#include <unordered_map>

#include "block.h"
#include "chunk.h"

class VoxelWorld {
public:
    const Chunk* findChunk(glm::ivec3 chunkPos) const;

    bool loadChunk(glm::ivec3 chunkPos, uint32_t revision, ChunkData&& data);
    bool unloadChunk(glm::ivec3 chunkPos);

    BlockData getBlock(glm::ivec3 worldPos) const;
    BlockQueryResult queryBlock(glm::ivec3 worldPos) const;

    template <typename Func>
    void forEachLoadedChunk(Func&& func) const {
        for (const auto& [chunkPos, _] : chunks_) {
            func(chunkPos);
        }
    }

private:
    std::unordered_map<glm::ivec3, std::unique_ptr<Chunk>> chunks_;
};
