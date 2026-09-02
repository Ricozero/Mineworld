#pragma once

#include <cstdint>
#include <glm/glm.hpp>

#include "chunk_data.h"

class ChunkGenerator {
public:
    static constexpr uint32_t INITIAL_REVISION = 1;

    static ChunkData generate(glm::ivec3 chunkPos);
};
