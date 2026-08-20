#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

class VoxelWorld;

// Bit (i*6+j) set when chunk face i and j are connected by open air (i<j).
// Face indices: 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z.
using ChunkFaceConnectivity = uint32_t;

inline constexpr size_t kMaxMeshVertices = UINT16_MAX;
inline constexpr size_t kMaxMeshIndices = UINT16_MAX;

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
    std::vector<float> vertexData;
    std::vector<uint16_t> indices;
    size_t vertexCount = 0;
    ChunkFaceConnectivity faceConnectivity = ~0u;
};

bool chunkFacesConnected(ChunkFaceConnectivity mask, int faceA, int faceB);

ChunkMesh buildChunkMesh(const VoxelWorld& voxelWorld, glm::ivec3 chunkPos);
