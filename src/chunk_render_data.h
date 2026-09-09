#pragma once

#include <cstdint>
#include <span>

#include "chunk_mesh_pool.h"

struct DrawableChunk {
    glm::ivec3 chunkPos{0};
    ChunkFaceConnectivity connectivity = kOpenChunkFaceConnectivity;
    ChunkMeshBinding binding;
};

struct ChunkRenderView {
    std::span<const DrawableChunk> chunks;
    uint64_t layoutRevision = 0;
    uint64_t topologyRevision = 0;
    uint64_t drawableRevision = 0;
};
