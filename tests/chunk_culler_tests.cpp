#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <climits>
#include <map>
#include <vector>

#include "chunk_culler.h"
#include "chunk_test_support.h"

namespace {

using namespace test_support;

bool facesConnected(const ChunkFaceConnectivity& connectivity, int from, int to) {
    return (connectivity[from] & (1u << to)) != 0;
}
using ChunkKey = std::array<int, 3>;

ChunkKey keyOf(glm::ivec3 chunkPos) {
    return ChunkKey{chunkPos.x, chunkPos.y, chunkPos.z};
}

struct ChunkList {
    ChunkList() : revision(nextRevision++) {}

    std::vector<DrawableChunk> chunks;
    uint64_t revision;

    static inline uint64_t nextRevision = 1;

    void add(glm::ivec3 chunkPos, ChunkFaceConnectivity connectivity) {
        ChunkMeshBinding binding;
        binding.vertexBuffer = 1;
        binding.vertexOffset = static_cast<uint32_t>(chunks.size()) * 4 + 1;
        binding.vertexCount = 4;
        chunks.push_back(DrawableChunk{chunkPos, connectivity, binding});
        revision = nextRevision++;
    }

    void removeLast() {
        chunks.pop_back();
        revision = nextRevision++;
    }

    ChunkRenderView view() const { return ChunkRenderView{chunks, revision, revision, revision}; }
};

ChunkFaceConnectivity pairConnectivity(int faceA, int faceB) {
    ChunkFaceConnectivity connectivity{};
    connectivity[faceA] = static_cast<uint8_t>(1u << faceB);
    connectivity[faceB] = static_cast<uint8_t>(1u << faceA);
    return connectivity;
}

Frustum uniformFrustum(float w) {
    Frustum frustum;
    for (glm::vec4& plane : frustum.planes) {
        plane = glm::vec4(0.0f, 0.0f, 0.0f, w);
    }
    return frustum;
}

glm::vec3 cameraInChunk(glm::ivec3 chunkPos) {
    return glm::vec3(chunkPos) * static_cast<float>(ChunkLayout::kSize) + static_cast<float>(ChunkLayout::kSize) * 0.5f;
}

bool chunkInFrustum(const Frustum& frustum, glm::ivec3 chunkPos) {
    const glm::vec3 min = glm::vec3(chunkPos) * static_cast<float>(ChunkLayout::kSize);
    return frustum.testAABB(min, min + static_cast<float>(ChunkLayout::kSize));
}

std::vector<ChunkKey> referenceVisible(const ChunkList& source, const Frustum& frustum, glm::vec3 cameraPosition) {
    std::map<ChunkKey, size_t> byPosition;
    for (size_t i = 0; i < source.chunks.size(); ++i) {
        byPosition.emplace(keyOf(source.chunks[i].chunkPos), i);
    }

    glm::ivec3 boundsMin(INT_MAX);
    glm::ivec3 boundsMax(INT_MIN);
    for (const DrawableChunk& chunk : source.chunks) {
        boundsMin = glm::min(boundsMin, chunk.chunkPos);
        boundsMax = glm::max(boundsMax, chunk.chunkPos);
    }
    const int64_t cells = (static_cast<int64_t>(boundsMax.x) - boundsMin.x + 3) *
                          (static_cast<int64_t>(boundsMax.y) - boundsMin.y + 3) *
                          (static_cast<int64_t>(boundsMax.z) - boundsMin.z + 3);

    std::vector<ChunkKey> visible;
    const glm::ivec3 cameraChunk = ChunkLayout::worldToChunk(glm::ivec3(glm::floor(cameraPosition)));
    const auto seed = byPosition.find(keyOf(cameraChunk));
    if (seed == byPosition.end() || cells > static_cast<int64_t>(ChunkCuller::kMaxGridCells)) {
        for (const DrawableChunk& chunk : source.chunks) {
            if (chunkInFrustum(frustum, chunk.chunkPos)) {
                visible.push_back(keyOf(chunk.chunkPos));
            }
        }
        std::sort(visible.begin(), visible.end());
        return visible;
    }

    std::vector<uint8_t> reached(source.chunks.size(), 0);
    reached[seed->second] = 0x40u;
    for (bool grew = true; grew;) {
        grew = false;
        for (size_t i = 0; i < source.chunks.size(); ++i) {
            for (int inFace = -1; inFace < 6; ++inFace) {
                const auto stateBit = static_cast<uint8_t>(inFace < 0 ? 0x40u : 1u << inFace);
                if ((reached[i] & stateBit) == 0) {
                    continue;
                }
                for (int outFace = 0; outFace < 6; ++outFace) {
                    if (inFace >= 0 && !facesConnected(source.chunks[i].connectivity, inFace, outFace)) {
                        continue;
                    }
                    const auto neighbor = byPosition.find(keyOf(source.chunks[i].chunkPos + kChunkFaceOffsets[outFace]));
                    if (neighbor == byPosition.end()) {
                        continue;
                    }
                    const auto enterBit = static_cast<uint8_t>(1u << kOppositeChunkFace[outFace]);
                    if ((reached[neighbor->second] & enterBit) != 0) {
                        continue;
                    }
                    reached[neighbor->second] |= enterBit;
                    grew = true;
                }
            }
        }
    }

    for (size_t i = 0; i < source.chunks.size(); ++i) {
        if (reached[i] != 0 && chunkInFrustum(frustum, source.chunks[i].chunkPos)) {
            visible.push_back(keyOf(source.chunks[i].chunkPos));
        }
    }
    std::sort(visible.begin(), visible.end());
    return visible;
}

void compareCull(ChunkCuller& culler, const ChunkList& source, const Frustum& frustum, glm::vec3 cameraPosition, const char* what) {
    SCOPED_TRACE(what);
    culler.cull(source.view(), frustum, cameraPosition);

    std::map<ChunkKey, uint32_t> expectedOffset;
    for (const DrawableChunk& chunk : source.chunks) {
        expectedOffset.emplace(keyOf(chunk.chunkPos), chunk.binding.vertexOffset);
    }

    std::vector<ChunkKey> actual;
    for (uint32_t visibleIndex : culler.visibleChunkIndices()) {
        ASSERT_LT(visibleIndex, source.chunks.size()) << "culler returned an out-of-range chunk index";
        const DrawableChunk& visible = source.chunks[visibleIndex];
        actual.push_back(keyOf(visible.chunkPos));
        const auto expected = expectedOffset.find(keyOf(visible.chunkPos));
        ASSERT_NE(expected, expectedOffset.end()) << "Visible chunk was never loaded";
        EXPECT_EQ(visible.binding.vertexOffset, expected->second);
    }

    std::sort(actual.begin(), actual.end());
    EXPECT_EQ(std::adjacent_find(actual.begin(), actual.end()), actual.end()) << "A chunk was emitted more than once";

    const std::vector<ChunkKey> expected = referenceVisible(source, frustum, cameraPosition);
    EXPECT_EQ(actual, expected);
}

Frustum halfSpaceFrustum() {
    Frustum frustum = uniformFrustum(1.0f);
    frustum.planes[0] = glm::vec4(1.0f, 0.0f, 0.0f, -1.0f);
    return frustum;
}

TEST(ChunkCullerTest, FrustumPlanes) {
    const Frustum slab = halfSpaceFrustum();
    EXPECT_TRUE(chunkInFrustum(slab, glm::ivec3(0, 0, 0))) << "a box straddling the plane is inside";
    EXPECT_TRUE(chunkInFrustum(slab, glm::ivec3(4, -3, 7))) << "a box well inside is inside";
    EXPECT_FALSE(chunkInFrustum(slab, glm::ivec3(-1, 0, 0))) << "the box ending on the plane is rejected";
    EXPECT_FALSE(chunkInFrustum(slab, glm::ivec3(-9, 2, 2))) << "a box far outside is rejected";
    EXPECT_FALSE(chunkInFrustum(uniformFrustum(-1.0f), glm::ivec3(0, 0, 0))) << "a closed frustum rejects everything";
}

TEST(ChunkCullerTest, ConnectivityCulling) {
    const Frustum open = uniformFrustum(1.0f);
    const Frustum closed = uniformFrustum(-1.0f);
    const Frustum halfSpace = halfSpaceFrustum();

    ChunkCuller culler;

    {
        ChunkList empty;
        culler.cull(empty.view(), open, glm::vec3(0.0f));
        EXPECT_TRUE(culler.visibleChunkIndices().empty()) << "nothing loaded draws nothing";
    }

    {
        ChunkList source;
        source.add(glm::ivec3(0, 0, 0), {});
        source.add(glm::ivec3(4000, 0, 4000), {});
        ASSERT_NO_FATAL_FAILURE(compareCull(culler, source, open, cameraInChunk(glm::ivec3(0, 0, 0)), "bounding box over the cell cap"));
        EXPECT_EQ(culler.visibleChunkIndices().size(), 2) << "the oversized box falls back to the frustum test";
        ASSERT_NO_FATAL_FAILURE(compareCull(culler, source, halfSpace, cameraInChunk(glm::ivec3(0, 0, 0)), "oversized box, half space"));

        source.removeLast();
        source.add(glm::ivec3(2, 0, 0), {});
        ASSERT_NO_FATAL_FAILURE(compareCull(culler, source, open, cameraInChunk(glm::ivec3(0, 0, 0)), "back under the cell cap"));
        EXPECT_EQ(culler.visibleChunkIndices().size(), 1) << "a small box uses the fill again";
    }

    {
        ChunkList source;
        for (int y = -1; y <= 1; ++y)
            for (int z = -1; z <= 1; ++z)
                for (int x = -1; x <= 1; ++x)
                    source.add(glm::ivec3(x, y, z), kOpenChunkFaceConnectivity);
        ASSERT_NO_FATAL_FAILURE(compareCull(culler, source, open, cameraInChunk(glm::ivec3(0)), "open 3x3x3"));
        EXPECT_EQ(culler.visibleChunkIndices().size(), 27) << "an open 3x3x3 is fully visible";
        ASSERT_FALSE(culler.visibleChunkIndices().empty()) << "the camera's chunk comes out first";
        ASSERT_EQ(source.chunks[culler.visibleChunkIndices()[0]].chunkPos, glm::ivec3(0)) << "the camera's chunk comes out first";
        ASSERT_NO_FATAL_FAILURE(compareCull(culler, source, closed, cameraInChunk(glm::ivec3(0)), "open 3x3x3, closed frustum"));
        EXPECT_TRUE(culler.visibleChunkIndices().empty()) << "a closed frustum leaves nothing to draw";
        ASSERT_NO_FATAL_FAILURE(compareCull(culler, source, halfSpace, cameraInChunk(glm::ivec3(0)), "open 3x3x3, half space"));
        EXPECT_EQ(culler.visibleChunkIndices().size(), 18) << "the half space keeps two of three chunk columns";
    }

    {
        ChunkList source;
        for (int y = -1; y <= 1; ++y)
            for (int z = -1; z <= 1; ++z)
                for (int x = -1; x <= 1; ++x)
                    source.add(glm::ivec3(x, y, z), {});
        ASSERT_NO_FATAL_FAILURE(compareCull(culler, source, open, cameraInChunk(glm::ivec3(0)), "sealed 3x3x3"));
        EXPECT_EQ(culler.visibleChunkIndices().size(), 7) << "sealed neighbours are drawn but nothing behind them";
    }

    {
        ChunkList source;
        for (int x = 0; x < 8; ++x) {
            source.add(glm::ivec3(x, 0, 0), pairConnectivity(0, 1));
        }
        source.add(glm::ivec3(4, 1, 0), kOpenChunkFaceConnectivity);
        source.add(glm::ivec3(20, 0, 0), kOpenChunkFaceConnectivity);
        ASSERT_NO_FATAL_FAILURE(compareCull(culler, source, open, cameraInChunk(glm::ivec3(0, 0, 0)), "x corridor"));
        EXPECT_EQ(culler.visibleChunkIndices().size(), 8) << "the corridor is drawn, the side chunk and the island are not";

        ASSERT_NO_FATAL_FAILURE(compareCull(culler, source, open, cameraInChunk(glm::ivec3(4, 0, 0)), "x corridor, mid camera"));
        EXPECT_EQ(culler.visibleChunkIndices().size(), 9) << "a mid-corridor camera also sees the side chunk";
    }

    {
        ChunkList source;
        for (int x = 0; x < 4; ++x) {
            source.add(glm::ivec3(x, 0, 0), {});
        }
        ASSERT_NO_FATAL_FAILURE(compareCull(culler, source, open, cameraInChunk(glm::ivec3(0, 8, 0)), "camera outside the loaded set"));
        EXPECT_EQ(culler.visibleChunkIndices().size(), 4) << "the fallback draws everything loaded";
        ASSERT_NO_FATAL_FAILURE(compareCull(culler, source, halfSpace, cameraInChunk(glm::ivec3(-40, 0, 0)), "fallback, half space"));
        EXPECT_EQ(culler.visibleChunkIndices().size(), 4) << "the fallback still honours the frustum";
        ASSERT_NO_FATAL_FAILURE(compareCull(culler, source, halfSpace, cameraInChunk(glm::ivec3(0, 0, 40)), "fallback from a third direction"));
    }

    {
        Rng rng{0x5EEDFACE1234ull};
        SCOPED_TRACE("Seed: 0x5EEDFACE1234");
        const glm::ivec3 origins[] = {{0, 0, 0}, {5, 1, 5}, {-7, -2, 3}, {30, 0, 30}, {-40, 1, -9}, {0, -3, 0}};
        for (int pass = 0; pass < 600; ++pass) {
            SCOPED_TRACE(testing::Message() << "Pass: " << pass);
            const glm::ivec3 origin = origins[pass % (sizeof(origins) / sizeof(origins[0]))];
            const glm::ivec3 extent(2 + static_cast<int>(rng.below(3)), 2 + static_cast<int>(rng.below(3)), 2 + static_cast<int>(rng.below(3)));

            ChunkList source;
            for (int y = 0; y < extent.y; ++y) {
                for (int z = 0; z < extent.z; ++z) {
                    for (int x = 0; x < extent.x; ++x) {
                        if (rng.below(100) < 20) {
                            continue;
                        }
                        ChunkFaceConnectivity mask{};
                        for (int from = 0; from < 6; ++from) {
                            for (int to = from + 1; to < 6; ++to) {
                                if (rng.below(100) < 35) {
                                    const auto pair = pairConnectivity(from, to);
                                    for (int face = 0; face < 6; ++face) {
                                        mask[face] |= pair[face];
                                    }
                                }
                            }
                        }
                        source.add(origin + glm::ivec3(x, y, z), mask);
                    }
                }
            }
            if (source.chunks.empty()) {
                continue;
            }

            const glm::ivec3 cameraChunk = rng.below(2) == 0
                                               ? source.chunks[rng.below(static_cast<uint32_t>(source.chunks.size()))].chunkPos
                                               : origin + glm::ivec3(0, -4, 0);
            ASSERT_NO_FATAL_FAILURE(compareCull(culler, source, rng.below(2) == 0 ? open : halfSpace, cameraInChunk(cameraChunk), "random chunk set"));
        }
    }
}

}  // namespace
