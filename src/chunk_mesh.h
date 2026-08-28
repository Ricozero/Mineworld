#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

#include "chunk.h"

class VoxelWorld;

inline constexpr size_t kMaxChunkMeshVertices = ChunkData::BLOCK_COUNT / 2 * 6 * 4;
static_assert(kMaxChunkMeshVertices <= 65536, "Chunk meshes are indexed with uint16 relative to the draw's base vertex");

struct ChunkVertex {
    uint8_t x = 0;
    uint8_t y = 0;
    uint8_t z = 0;
    uint8_t positionPadding = 0;
    uint32_t abgr = 0xff000000u;
};
static_assert(sizeof(ChunkVertex) == 8, "ChunkVertex must match the packed bgfx vertex layout");

// Bit (i*6+j) set when chunk face i and j are connected by open air (i<j).
// Face indices: 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z.
using ChunkFaceConnectivity = uint32_t;

inline constexpr glm::ivec3 kChunkFaceOffsets[6] = {
    {1, 0, 0},
    {-1, 0, 0},
    {0, 1, 0},
    {0, -1, 0},
    {0, 0, 1},
    {0, 0, -1},
};
inline constexpr int kOppositeChunkFace[6] = {1, 0, 3, 2, 5, 4};

struct ChunkMesh {
    std::vector<ChunkVertex> vertices;
    ChunkFaceConnectivity faceConnectivity = ~0u;
};

bool chunkFacesConnected(ChunkFaceConnectivity mask, int faceA, int faceB);

ChunkMesh buildChunkMesh(const VoxelWorld& voxelWorld, glm::ivec3 chunkPos);
