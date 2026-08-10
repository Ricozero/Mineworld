#pragma once

#include <glm/glm.hpp>

#include "chunk.h"

class ChunkGenerator {
public:
    static ChunkData generate(glm::ivec3 chunkPos);
    static bool isChunkInBounds(glm::ivec3 chunkPos);

private:
    static BlockData generateBlock(glm::ivec3 worldPos);
    static bool isBlockInBounds(glm::ivec3 worldPos);
};
