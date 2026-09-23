#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <glm/packing.hpp>
#include <queue>
#include <vector>

#include "chunk_mesh.h"
#include "chunk_test_support.h"
#include "voxel_world.h"

namespace {

using namespace test_support;

constexpr size_t kBlockCount = ChunkLayout::kBlockCount;
constexpr int kSize = 16;
constexpr int kSizeBits = 4;
constexpr int kXStride = 1;
constexpr int kZStride = kSize;
constexpr int kYStride = kSize * kSize;

constexpr size_t blockIndex(int x, int y, int z) {
    return (static_cast<size_t>(y) << (kSizeBits * 2)) | (static_cast<size_t>(z) << kSizeBits) | static_cast<size_t>(x);
}

constexpr int bitIndex(int faceA, int faceB) {
    return faceA < faceB ? faceA * 6 + faceB : faceB * 6 + faceA;
}

struct NeighborStep {
    int indexOffset;
    int axis;
    int boundary;
};

constexpr std::array<NeighborStep, 6> kNeighborSteps = {{
    {kXStride, 0, kSize - 1},
    {-kXStride, 0, 0},
    {kYStride, 1, kSize - 1},
    {-kYStride, 1, 0},
    {kZStride, 2, kSize - 1},
    {-kZStride, 2, 0},
}};

uint32_t referenceConnectivity(const std::array<uint8_t, kBlockCount>& air) {
    uint8_t reachable[6] = {};
    std::array<uint8_t, kBlockCount> visited{};
    std::queue<size_t> q;

    for (int startFace = 0; startFace < 6; ++startFace) {
        visited.fill(0);
        q = {};
        auto enqueue = [&](size_t index) {
            if (air[index] && !visited[index]) {
                visited[index] = 1;
                q.push(index);
            }
        };

        for (int a = 0; a < kSize; ++a) {
            for (int b = 0; b < kSize; ++b) {
                switch (startFace) {
                    case 0: enqueue(blockIndex(kSize - 1, a, b)); break;
                    case 1: enqueue(blockIndex(0, a, b)); break;
                    case 2: enqueue(blockIndex(a, kSize - 1, b)); break;
                    case 3: enqueue(blockIndex(a, 0, b)); break;
                    case 4: enqueue(blockIndex(a, b, kSize - 1)); break;
                    case 5: enqueue(blockIndex(a, b, 0)); break;
                }
            }
        }

        while (!q.empty()) {
            const size_t index = q.front();
            q.pop();
            const int x = static_cast<int>(index & (kSize - 1));
            const int y = static_cast<int>(index >> (kSizeBits * 2));
            const int z = static_cast<int>((index >> kSizeBits) & (kSize - 1));
            const int pos[3] = {x, y, z};

            if (x == kSize - 1) reachable[startFace] |= (1 << 0);
            if (x == 0) reachable[startFace] |= (1 << 1);
            if (y == kSize - 1) reachable[startFace] |= (1 << 2);
            if (y == 0) reachable[startFace] |= (1 << 3);
            if (z == kSize - 1) reachable[startFace] |= (1 << 4);
            if (z == 0) reachable[startFace] |= (1 << 5);

            for (const NeighborStep& step : kNeighborSteps) {
                if (pos[step.axis] == step.boundary) continue;
                enqueue(static_cast<size_t>(static_cast<int>(index) + step.indexOffset));
            }
        }
    }

    uint32_t mask = 0;
    for (int from = 0; from < 6; ++from)
        for (int to = from + 1; to < 6; ++to)
            if ((reachable[from] >> to) & 1)
                mask |= uint32_t{1} << bitIndex(from, to);
    return mask;
}

constexpr glm::vec3 kBlockAlbedo[static_cast<size_t>(BlockType::Count)] = {
    glm::vec3(0.0f),                 // Air
    glm::vec3(0.48f, 0.50f, 0.53f),  // Stone
    glm::vec3(0.26f, 0.17f, 0.10f),  // Dirt
    glm::vec3(0.24f, 0.58f, 0.22f),  // Grass
    glm::vec3(0.50f, 0.31f, 0.14f),  // Wood
    glm::vec3(0.16f, 0.45f, 0.18f),  // Leaves
    glm::vec3(0.20f, 0.42f, 0.85f),  // Water
    glm::vec3(0.78f, 0.68f, 0.42f),  // Sand
};

constexpr std::array<std::array<glm::ivec3, 4>, 6> kFaceCorners = {{
    {glm::ivec3(1, 0, 0), glm::ivec3(1, 1, 0), glm::ivec3(1, 1, 1), glm::ivec3(1, 0, 1)},
    {glm::ivec3(0, 0, 1), glm::ivec3(0, 1, 1), glm::ivec3(0, 1, 0), glm::ivec3(0, 0, 0)},
    {glm::ivec3(0, 1, 1), glm::ivec3(1, 1, 1), glm::ivec3(1, 1, 0), glm::ivec3(0, 1, 0)},
    {glm::ivec3(0, 0, 0), glm::ivec3(1, 0, 0), glm::ivec3(1, 0, 1), glm::ivec3(0, 0, 1)},
    {glm::ivec3(1, 0, 1), glm::ivec3(1, 1, 1), glm::ivec3(0, 1, 1), glm::ivec3(0, 0, 1)},
    {glm::ivec3(0, 0, 0), glm::ivec3(0, 1, 0), glm::ivec3(1, 1, 0), glm::ivec3(1, 0, 0)},
}};

uint32_t referenceFaceColor(BlockType type, int face) {
    const glm::vec3 lightDirection = glm::normalize(glm::vec3(0.4f, 1.0f, 0.55f));
    constexpr float kAmbient = 0.3f;
    const float wrapped = glm::dot(glm::vec3(kChunkFaceOffsets[face]), lightDirection) * 0.5f + 0.5f;
    const float shade = kAmbient + (1.0f - kAmbient) * wrapped;
    return glm::packUnorm4x8(glm::vec4(kBlockAlbedo[static_cast<size_t>(type)] * shade, 1.0f));
}

BlockData blockAcross(const VoxelWorld& world, glm::ivec3 chunkPos, glm::ivec3 localPos) {
    for (int axis = 0; axis < 3; ++axis) {
        if (localPos[axis] < 0) {
            localPos[axis] += kSize;
            chunkPos[axis] -= 1;
        } else if (localPos[axis] >= kSize) {
            localPos[axis] -= kSize;
            chunkPos[axis] += 1;
        }
    }
    const Chunk* chunk = world.findChunk(chunkPos);
    return chunk == nullptr ? BlockData{} : chunk->getBlock(localPos);
}

std::vector<ChunkVertex> referenceMesh(const VoxelWorld& world, glm::ivec3 chunkPos) {
    std::vector<ChunkVertex> vertices;
    const Chunk* chunk = world.findChunk(chunkPos);
    if (chunk == nullptr || chunk->isEmpty()) {
        return vertices;
    }

    for (int y = 0; y < kSize; ++y) {
        for (int z = 0; z < kSize; ++z) {
            for (int x = 0; x < kSize; ++x) {
                const glm::ivec3 localPos(x, y, z);
                const BlockData block = chunk->getBlock(blockIndex(x, y, z));
                if (block.type == BlockType::Air) {
                    continue;
                }
                for (int face = 0; face < 6; ++face) {
                    if (blockAcross(world, chunkPos, localPos + kChunkFaceOffsets[face]).type != BlockType::Air) {
                        continue;
                    }
                    const uint32_t color = referenceFaceColor(block.type, face);
                    for (const glm::ivec3& corner : kFaceCorners[static_cast<size_t>(face)]) {
                        const glm::ivec3 vertexPos = localPos + corner;
                        vertices.push_back(ChunkVertex{static_cast<uint8_t>(vertexPos.x),
                                                       static_cast<uint8_t>(vertexPos.y),
                                                       static_cast<uint8_t>(vertexPos.z), 0, color});
                    }
                }
            }
        }
    }
    return vertices;
}

ChunkFaceConnectivity connectivityFromMask(uint32_t mask) {
    ChunkFaceConnectivity connectivity{};
    for (int from = 0; from < 6; ++from) {
        for (int to = from + 1; to < 6; ++to) {
            if ((mask & (uint32_t{1} << bitIndex(from, to))) != 0) {
                connectivity[from] |= static_cast<uint8_t>(1u << to);
                connectivity[to] |= static_cast<uint8_t>(1u << from);
            }
        }
    }
    return connectivity;
}

ChunkFaceConnectivity referenceMeshConnectivity(const VoxelWorld& world, glm::ivec3 chunkPos) {
    const Chunk* chunk = world.findChunk(chunkPos);
    if (chunk == nullptr || chunk->isEmpty()) {
        return kOpenChunkFaceConnectivity;
    }
    std::array<uint8_t, kBlockCount> air{};
    for (size_t index = 0; index < kBlockCount; ++index) {
        air[index] = chunk->getBlock(index).type == BlockType::Air ? 1 : 0;
    }
    return connectivityFromMask(referenceConnectivity(air));
}

using MeshQuad = std::array<uint64_t, 4>;

uint64_t encodeVertex(const ChunkVertex& vertex) {
    return static_cast<uint64_t>(vertex.x) | (static_cast<uint64_t>(vertex.y) << 8) |
           (static_cast<uint64_t>(vertex.z) << 16) | (static_cast<uint64_t>(vertex.positionPadding) << 24) |
           (static_cast<uint64_t>(vertex.abgr) << 32);
}

std::vector<MeshQuad> sortedQuads(const std::vector<ChunkVertex>& vertices) {
    std::vector<MeshQuad> quads;
    for (size_t base = 0; base + 4 <= vertices.size(); base += 4) {
        quads.push_back(MeshQuad{encodeVertex(vertices[base + 0]), encodeVertex(vertices[base + 1]),
                                 encodeVertex(vertices[base + 2]), encodeVertex(vertices[base + 3])});
    }
    std::sort(quads.begin(), quads.end());
    return quads;
}

void loadUniform(VoxelWorld& world, glm::ivec3 chunkPos, BlockData block) {
    world.loadChunk(chunkPos, 1, ChunkData{block});
}

void loadPalette(VoxelWorld& world, glm::ivec3 chunkPos, const std::array<BlockData, kBlockCount>& blocks) {
    ChunkData data;
    data.set(0, BlockData{BlockType::Stone});
    for (size_t index = 0; index < kBlockCount; ++index) {
        data.set(index, blocks[index]);
    }
    world.loadChunk(chunkPos, 1, std::move(data));
}

void compareMesh(ChunkMesh& scratch, const VoxelWorld& world, glm::ivec3 chunkPos, const char* what) {
    SCOPED_TRACE(what);
    buildChunkMesh(world, chunkPos, scratch);
    ASSERT_EQ(scratch.vertices.size() % 4, 0u) << "Vertices must form whole quads";

    const std::vector<MeshQuad> expected = sortedQuads(referenceMesh(world, chunkPos));
    const std::vector<MeshQuad> actual = sortedQuads(scratch.vertices);
    EXPECT_EQ(actual, expected);

    const ChunkFaceConnectivity expectedConnectivity = referenceMeshConnectivity(world, chunkPos);
    EXPECT_EQ(scratch.connectivity, expectedConnectivity);
}

TEST(ChunkMeshTest, MatchesReferenceMesher) {
    SCOPED_TRACE("Seed: 0x51ED270FA11");
    Rng rng{0x51ED270FA11ull};
    const glm::ivec3 center(3, -2, 7);
    ChunkMesh scratch;

    {
        VoxelWorld world;
        ASSERT_NO_FATAL_FAILURE(compareMesh(scratch, world, center, "missing chunk"));
    }
    {
        VoxelWorld world;
        loadUniform(world, center, BlockData{});
        ASSERT_NO_FATAL_FAILURE(compareMesh(scratch, world, center, "uniform air chunk"));
    }
    {
        VoxelWorld world;
        loadUniform(world, center, BlockData{BlockType::Stone});
        ASSERT_NO_FATAL_FAILURE(compareMesh(scratch, world, center, "uniform solid chunk without neighbours"));
    }
    {
        VoxelWorld world;
        loadUniform(world, center, BlockData{BlockType::Stone});
        for (const glm::ivec3& offset : kChunkFaceOffsets) {
            loadUniform(world, center + offset, BlockData{BlockType::Dirt});
        }
        ASSERT_NO_FATAL_FAILURE(compareMesh(scratch, world, center, "uniform solid chunk sealed by neighbours"));
    }
    {
        VoxelWorld world;
        loadUniform(world, center, BlockData{BlockType::Stone});
        loadUniform(world, center + kChunkFaceOffsets[2], BlockData{});
        loadUniform(world, center + kChunkFaceOffsets[5], BlockData{BlockType::Sand});
        ASSERT_NO_FATAL_FAILURE(compareMesh(scratch, world, center, "uniform solid chunk with mixed neighbours"));
    }
    {
        std::array<BlockData, kBlockCount> blocks;
        blocks.fill(BlockData{BlockType::Wood, BlockOrientation::Up});
        VoxelWorld world;
        loadPalette(world, center, blocks);
        ASSERT_NO_FATAL_FAILURE(compareMesh(scratch, world, center, "solid palette chunk"));
    }
    {
        std::array<BlockData, kBlockCount> blocks;
        blocks.fill(BlockData{});
        for (int y = 0; y < 8; ++y) {
            for (int z = 0; z < kSize; ++z) {
                for (int x = 0; x < kSize; ++x) {
                    blocks[blockIndex(x, y, z)] = BlockData{BlockType::Grass};
                }
            }
        }
        for (int y = 0; y < 8; ++y) {
            blocks[blockIndex(4, y, 4)] = BlockData{};
        }
        VoxelWorld world;
        loadPalette(world, center, blocks);
        loadUniform(world, center + kChunkFaceOffsets[3], BlockData{BlockType::Stone});
        ASSERT_NO_FATAL_FAILURE(compareMesh(scratch, world, center, "floor with a shaft"));
    }

    std::array<BlockData, kBlockCount> blocks;
    for (int density = 5; density < 100; density += 17) {
        for (int trial = 0; trial < 12; ++trial) {
            SCOPED_TRACE(testing::Message() << "Density: " << density << ", trial: " << trial);
            VoxelWorld world;
            for (size_t index = 0; index < kBlockCount; ++index) {
                if (rng.below(100) >= static_cast<uint32_t>(density)) {
                    blocks[index] = BlockData{};
                    continue;
                }
                blocks[index] = BlockData{
                    static_cast<BlockType>(1 + rng.below(static_cast<uint32_t>(BlockType::Count) - 1)),
                    static_cast<BlockOrientation>(rng.below(static_cast<uint32_t>(BlockOrientation::Count)))};
            }
            loadPalette(world, center, blocks);

            for (const glm::ivec3& offset : kChunkFaceOffsets) {
                switch (rng.below(4)) {
                    case 0: break;
                    case 1: loadUniform(world, center + offset, BlockData{}); break;
                    case 2: loadUniform(world, center + offset, BlockData{BlockType::Stone}); break;
                    default: {
                        std::array<BlockData, kBlockCount> neighborBlocks;
                        for (size_t index = 0; index < kBlockCount; ++index) {
                            neighborBlocks[index] = rng.below(2) == 0 ? BlockData{} : BlockData{BlockType::Dirt};
                        }
                        loadPalette(world, center + offset, neighborBlocks);
                        break;
                    }
                }
            }
            ASSERT_NO_FATAL_FAILURE(compareMesh(scratch, world, center, "random chunk"));
        }
    }
}

}  // namespace
