#pragma once

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

#include "chunk_layout.h"

class VoxelWorld;

inline constexpr size_t kMaxChunkMeshVertices = ChunkLayout::BLOCK_COUNT / 2 * 6 * 4;
static_assert(kMaxChunkMeshVertices <= 65536, "Chunk meshes are indexed with uint16 relative to the draw's base vertex");

struct ChunkVertex {
    uint8_t x = 0;
    uint8_t y = 0;
    uint8_t z = 0;
    uint8_t positionPadding = 0;
    uint32_t abgr = 0xff000000u;
};
static_assert(sizeof(ChunkVertex) == 8, "ChunkVertex must match the packed bgfx vertex layout");

using ChunkFaceConnectivity = std::array<uint8_t, 6>;
constexpr uint8_t kAllFaces = 0x3Fu;
inline constexpr ChunkFaceConnectivity kOpenChunkFaceConnectivity = [] {
    ChunkFaceConnectivity connectivity{};
    for (int face = 0; face < 6; ++face) {
        connectivity[face] = static_cast<uint8_t>(kAllFaces & ~(1u << face));
    }
    return connectivity;
}();

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
    ChunkFaceConnectivity connectivity = kOpenChunkFaceConnectivity;
};
void buildChunkMesh(const VoxelWorld& voxelWorld, glm::ivec3 chunkPos, ChunkMesh& out);
