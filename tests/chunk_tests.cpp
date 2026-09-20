#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <glm/packing.hpp>
#include <map>
#include <queue>
#include <set>
#include <vector>

#include "chunk.h"
#include "chunk_culler.h"
#include "chunk_mesh.h"
#include "server_chunk_manager.h"
#include "voxel_world.h"

namespace {

struct BlobInfo {
    uint8_t bits = 0;
    size_t paletteSize = 0;
    size_t bytes = 0;
};

BlobInfo inspect(const ChunkData& storage) {
    std::vector<uint8_t> blob;
    storage.serialize(blob);
    BlobInfo info;
    info.bytes = blob.size();
    if (!blob.empty() && blob[0] == static_cast<uint8_t>(ChunkData::Format::Palette)) {
        info.bits = blob[1];
        info.paletteSize = static_cast<size_t>(blob[2]) + 1;
    }
    return info;
}

void check(bool ok, const char* what) {
    if (!ok) {
        ADD_FAILURE() << what;
    }
}

struct Rng {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<uint32_t>(state >> 32);
    }
    uint32_t below(uint32_t bound) { return next() % bound; }
};

constexpr size_t N = ChunkLayout::BLOCK_COUNT;

BlockData randomBlock(Rng& rng, uint32_t typeCount, uint32_t orientationCount) {
    return BlockData{static_cast<BlockType>(rng.below(typeCount)), static_cast<BlockOrientation>(rng.below(orientationCount))};
}

size_t countNonAir(const std::vector<uint16_t>& ref) {
    size_t count = 0;
    for (uint16_t packed : ref) {
        if (unpackBlock(packed).type != BlockType::Air) {
            ++count;
        }
    }
    return count;
}

bool sameContents(const ChunkData& storage, const std::vector<uint16_t>& ref) {
    for (size_t i = 0; i < N; ++i) {
        if (packBlock(storage.get(i)) != ref[i]) {
            std::printf("  mismatch at %zu: got %04x want %04x\n", i, packBlock(storage.get(i)), ref[i]);
            return false;
        }
    }
    return true;
}

void testStorage(uint32_t typeCount, uint32_t orientationCount, uint64_t seed, const char* label) {
    Rng rng{seed};
    ChunkData storage;
    std::vector<uint16_t> ref(N, packBlock(BlockData{}));

    check(storage.isUniform(), "fresh storage is uniform");
    check(storage.blockCount() == 0, "fresh storage has no solid blocks");
    check(sameContents(storage, ref), "fresh storage reads as air");

    for (int iteration = 0; iteration < 400000; ++iteration) {
        const size_t index = rng.below(static_cast<uint32_t>(N));
        const BlockData block = randomBlock(rng, typeCount, orientationCount);
        const uint16_t packed = packBlock(block);
        const bool changed = storage.set(index, block);
        check(changed == (ref[index] != packed), "set() reports whether it changed anything");
        ref[index] = packed;

        if (iteration % 20000 == 0) {
            check(sameContents(storage, ref), "contents match the reference");
            check(storage.blockCount() == countNonAir(ref), "nonAirCount matches the reference");

            std::vector<uint8_t> blob;
            storage.serialize(blob);
            ChunkData restored;
            check(ChunkData::deserialize(blob, restored), "serialize output round-trips");
            check(sameContents(restored, ref), "round-tripped contents match");
            check(restored.blockCount() == countNonAir(ref), "round-tripped nonAirCount matches");
            check(restored.isUniform() == storage.isUniform(), "round-tripped uniform flag matches");

            storage.optimize();
            check(sameContents(storage, ref), "optimize() preserves contents");
            check(storage.blockCount() == countNonAir(ref), "optimize() preserves nonAirCount");
            const size_t distinct = [&] {
                std::array<bool, 256> seen{};
                size_t total = 0;
                for (uint16_t packedRef : ref) {
                    const size_t key = (static_cast<size_t>(packedRef >> 8) * 6) + (packedRef & 0xFF);
                    if (!seen[key]) {
                        seen[key] = true;
                        ++total;
                    }
                }
                return total;
            }();
            if (distinct == 1) {
                check(storage.isUniform(), "optimize() collapses a single-block chunk");
            } else {
                const BlobInfo info = inspect(storage);
                check(info.paletteSize == distinct, "optimize() keeps exactly the used palette entries");
                check(static_cast<size_t>(1) << info.bits >= distinct, "index width addresses the palette");
                check(info.bits / 2 == 0 || (static_cast<size_t>(1) << (info.bits / 2)) < distinct,
                      "index width is the smallest that fits");
            }
        }
    }
    const BlobInfo summary = inspect(storage);
    std::printf("  %s: bits=%u palette=%zu wire=%zu bytes\n", label, summary.bits, summary.paletteSize, summary.bytes);
}

TEST(ChunkDataTest, RandomizedStorageMatchesReference) {
    testStorage(2, 1, 1, "2 blocks");
    testStorage(3, 1, 2, "3 blocks");
    testStorage(5, 2, 3, "10 blocks");
    testStorage(static_cast<uint32_t>(BlockType::Count), static_cast<uint32_t>(BlockOrientation::Count), 4, "48 blocks");
}

TEST(ChunkDataTest, UniformAndMalformedBlobs) {
    ChunkData storage;
    storage.fill(BlockData{BlockType::Stone, BlockOrientation::North});
    check(storage.isUniform(), "fill() produces a uniform chunk");
    check(storage.blockCount() == N, "a uniform solid chunk counts every block");
    check(inspect(storage).bits == 0, "a uniform chunk keeps the uniform form");

    std::vector<uint8_t> blob;
    storage.serialize(blob);
    check(blob.size() == 3, "a uniform chunk serializes to 3 bytes");
    ChunkData restored;
    check(ChunkData::deserialize(blob, restored), "uniform blob round-trips");
    check(restored.isUniform() && packBlock(restored.uniformBlock()) == packBlock(storage.uniformBlock()), "uniform blob keeps its block");

    storage.set(100, BlockData{BlockType::Dirt, BlockOrientation::North});
    check(!storage.isUniform(), "editing a uniform chunk leaves the uniform form");
    storage.set(100, BlockData{BlockType::Stone, BlockOrientation::North});
    storage.optimize();
    check(storage.isUniform(), "optimize() returns to the uniform form");

    ChunkData sink;
    check(!ChunkData::deserialize({}, sink), "an empty blob is rejected");
    check(!ChunkData::deserialize(std::vector<uint8_t>{9, 0, 0}, sink), "an unknown format byte is rejected");
    check(!ChunkData::deserialize(std::vector<uint8_t>{0, 0}, sink), "a truncated uniform blob is rejected");
    check(!ChunkData::deserialize(std::vector<uint8_t>{0, 0, static_cast<uint8_t>(BlockType::Count)}, sink), "a uniform blob with an invalid block is rejected");

    std::vector<uint8_t> badBits{1, 3, 0, 0, 0};
    badBits.resize(3 + 2 + N * 3 / 8, 0);
    check(!ChunkData::deserialize(badBits, sink), "an unsupported index width is rejected");

    std::vector<uint8_t> tooBig{1, 1, 2};
    tooBig.resize(3 + 3 * 2 + N / 8, 0);
    check(!ChunkData::deserialize(tooBig, sink), "a palette larger than the index width allows is rejected");

    std::vector<uint8_t> shortBlob{1, 1, 1, 0, 0, 0, 0};
    check(!ChunkData::deserialize(shortBlob, sink), "a truncated palette blob is rejected");

    std::vector<uint8_t> outOfRange{1, 1, 0, 0, 0};
    outOfRange.resize(3 + 2 + N / 8, 0);
    outOfRange.back() = 0x80;
    check(!ChunkData::deserialize(outOfRange, sink), "an out-of-range index is rejected");

    std::vector<uint8_t> badEntry{1, 1, 1, 0, 0, static_cast<uint8_t>(BlockType::Count), 0};
    badEntry.resize(3 + 4 + N / 8, 0);
    check(!ChunkData::deserialize(badEntry, sink), "an invalid palette entry is rejected");
}

constexpr int SIZE = 16;
constexpr int SIZE_BITS = 4;
constexpr int X_STRIDE = 1;
constexpr int Z_STRIDE = SIZE;
constexpr int Y_STRIDE = SIZE * SIZE;

constexpr size_t blockIndex(int x, int y, int z) {
    return (static_cast<size_t>(y) << (SIZE_BITS * 2)) | (static_cast<size_t>(z) << SIZE_BITS) |
           static_cast<size_t>(x);
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
    {X_STRIDE, 0, SIZE - 1},
    {-X_STRIDE, 0, 0},
    {Y_STRIDE, 1, SIZE - 1},
    {-Y_STRIDE, 1, 0},
    {Z_STRIDE, 2, SIZE - 1},
    {-Z_STRIDE, 2, 0},
}};

uint32_t referenceConnectivity(const std::array<uint8_t, N>& air) {
    uint8_t reachable[6] = {};
    std::array<uint8_t, N> visited{};
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

        for (int a = 0; a < SIZE; ++a) {
            for (int b = 0; b < SIZE; ++b) {
                switch (startFace) {
                    case 0: enqueue(blockIndex(SIZE - 1, a, b)); break;
                    case 1: enqueue(blockIndex(0, a, b)); break;
                    case 2: enqueue(blockIndex(a, SIZE - 1, b)); break;
                    case 3: enqueue(blockIndex(a, 0, b)); break;
                    case 4: enqueue(blockIndex(a, b, SIZE - 1)); break;
                    case 5: enqueue(blockIndex(a, b, 0)); break;
                }
            }
        }

        while (!q.empty()) {
            const size_t index = q.front();
            q.pop();
            const int x = static_cast<int>(index & (SIZE - 1));
            const int y = static_cast<int>(index >> (SIZE_BITS * 2));
            const int z = static_cast<int>((index >> SIZE_BITS) & (SIZE - 1));
            const int pos[3] = {x, y, z};

            if (x == SIZE - 1) reachable[startFace] |= (1 << 0);
            if (x == 0) reachable[startFace] |= (1 << 1);
            if (y == SIZE - 1) reachable[startFace] |= (1 << 2);
            if (y == 0) reachable[startFace] |= (1 << 3);
            if (z == SIZE - 1) reachable[startFace] |= (1 << 4);
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
    constexpr float ambient = 0.3f;
    const float wrapped = glm::dot(glm::vec3(kChunkFaceOffsets[face]), lightDirection) * 0.5f + 0.5f;
    const float shade = ambient + (1.0f - ambient) * wrapped;
    return glm::packUnorm4x8(glm::vec4(kBlockAlbedo[static_cast<size_t>(type)] * shade, 1.0f));
}

BlockData blockAcross(const VoxelWorld& world, glm::ivec3 chunkPos, glm::ivec3 localPos) {
    for (int axis = 0; axis < 3; ++axis) {
        if (localPos[axis] < 0) {
            localPos[axis] += SIZE;
            chunkPos[axis] -= 1;
        } else if (localPos[axis] >= SIZE) {
            localPos[axis] -= SIZE;
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

    for (int y = 0; y < SIZE; ++y) {
        for (int z = 0; z < SIZE; ++z) {
            for (int x = 0; x < SIZE; ++x) {
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

bool facesConnected(const ChunkFaceConnectivity& connectivity, int from, int to) {
    return (connectivity[from] & (1u << to)) != 0;
}

ChunkFaceConnectivity referenceMeshConnectivity(const VoxelWorld& world, glm::ivec3 chunkPos) {
    const Chunk* chunk = world.findChunk(chunkPos);
    if (chunk == nullptr || chunk->isEmpty()) {
        return kOpenChunkFaceConnectivity;
    }
    std::array<uint8_t, N> air{};
    for (size_t index = 0; index < N; ++index) {
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

void loadPalette(VoxelWorld& world, glm::ivec3 chunkPos, const std::array<BlockData, N>& blocks) {
    ChunkData data;
    data.set(0, BlockData{BlockType::Stone});
    for (size_t index = 0; index < N; ++index) {
        data.set(index, blocks[index]);
    }
    world.loadChunk(chunkPos, 1, std::move(data));
}

void compareMesh(ChunkMesh& scratch, const VoxelWorld& world, glm::ivec3 chunkPos, const char* what) {
    buildChunkMesh(world, chunkPos, scratch);

    if (scratch.vertices.size() % 4 != 0) {
        ADD_FAILURE() << what << " emitted " << scratch.vertices.size() << " vertices, not a whole number of quads";
        return;
    }

    const std::vector<MeshQuad> expected = sortedQuads(referenceMesh(world, chunkPos));
    const std::vector<MeshQuad> actual = sortedQuads(scratch.vertices);
    if (expected != actual) {
        ADD_FAILURE() << what << " quads (reference " << expected.size() << ", mesher " << actual.size() << ')';
    }

    const ChunkFaceConnectivity expectedConnectivity = referenceMeshConnectivity(world, chunkPos);
    if (scratch.connectivity != expectedConnectivity) {
        ADD_FAILURE() << what << " connectivity differs from the reference";
    }
}

TEST(ChunkMeshTest, MatchesReferenceMesher) {
    Rng rng{0x51ED270FA11ull};
    const glm::ivec3 center(3, -2, 7);
    ChunkMesh scratch;

    {
        VoxelWorld world;
        compareMesh(scratch, world, center, "missing chunk");
    }
    {
        VoxelWorld world;
        loadUniform(world, center, BlockData{});
        compareMesh(scratch, world, center, "uniform air chunk");
    }
    {
        VoxelWorld world;
        loadUniform(world, center, BlockData{BlockType::Stone});
        compareMesh(scratch, world, center, "uniform solid chunk without neighbours");
    }
    {
        VoxelWorld world;
        loadUniform(world, center, BlockData{BlockType::Stone});
        for (const glm::ivec3& offset : kChunkFaceOffsets) {
            loadUniform(world, center + offset, BlockData{BlockType::Dirt});
        }
        compareMesh(scratch, world, center, "uniform solid chunk sealed by neighbours");
    }
    {
        VoxelWorld world;
        loadUniform(world, center, BlockData{BlockType::Stone});
        loadUniform(world, center + kChunkFaceOffsets[2], BlockData{});
        loadUniform(world, center + kChunkFaceOffsets[5], BlockData{BlockType::Sand});
        compareMesh(scratch, world, center, "uniform solid chunk with mixed neighbours");
    }
    {
        std::array<BlockData, N> blocks;
        blocks.fill(BlockData{BlockType::Wood, BlockOrientation::Up});
        VoxelWorld world;
        loadPalette(world, center, blocks);
        compareMesh(scratch, world, center, "solid palette chunk");
    }
    {
        std::array<BlockData, N> blocks;
        blocks.fill(BlockData{});
        for (int y = 0; y < 8; ++y) {
            for (int z = 0; z < SIZE; ++z) {
                for (int x = 0; x < SIZE; ++x) {
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
        compareMesh(scratch, world, center, "floor with a shaft");
    }

    std::array<BlockData, N> blocks;
    for (int density = 5; density < 100; density += 17) {
        for (int trial = 0; trial < 12; ++trial) {
            VoxelWorld world;
            for (size_t index = 0; index < N; ++index) {
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
                        std::array<BlockData, N> neighborBlocks;
                        for (size_t index = 0; index < N; ++index) {
                            neighborBlocks[index] = rng.below(2) == 0 ? BlockData{} : BlockData{BlockType::Dirt};
                        }
                        loadPalette(world, center + offset, neighborBlocks);
                        break;
                    }
                }
            }
            compareMesh(scratch, world, center, "random chunk");
        }
    }
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
    return glm::vec3(chunkPos) * static_cast<float>(ChunkLayout::SIZE) + static_cast<float>(ChunkLayout::SIZE) * 0.5f;
}

bool chunkInFrustum(const Frustum& frustum, glm::ivec3 chunkPos) {
    const glm::vec3 min = glm::vec3(chunkPos) * static_cast<float>(ChunkLayout::SIZE);
    return frustum.testAABB(min, min + static_cast<float>(ChunkLayout::SIZE));
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
    if (seed == byPosition.end() || cells > static_cast<int64_t>(ChunkCuller::MAX_GRID_CELLS)) {
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
    culler.cull(source.view(), frustum, cameraPosition);

    std::map<ChunkKey, uint32_t> expectedOffset;
    for (const DrawableChunk& chunk : source.chunks) {
        expectedOffset.emplace(keyOf(chunk.chunkPos), chunk.binding.vertexOffset);
    }

    std::vector<ChunkKey> actual;
    for (uint32_t visibleIndex : culler.visibleChunkIndices()) {
        check(visibleIndex < source.chunks.size(), "culler returned an out-of-range chunk index");
        if (visibleIndex >= source.chunks.size()) {
            return;
        }
        const DrawableChunk& visible = source.chunks[visibleIndex];
        actual.push_back(keyOf(visible.chunkPos));
        const auto expected = expectedOffset.find(keyOf(visible.chunkPos));
        if (expected == expectedOffset.end()) {
            ADD_FAILURE() << what << " (visible chunk " << visible.chunkPos.x << ',' << visible.chunkPos.y << ',' << visible.chunkPos.z << " was never loaded)";
            return;
        }
        if (visible.binding.vertexOffset != expected->second) {
            ADD_FAILURE() << what << " (chunk " << visible.chunkPos.x << ',' << visible.chunkPos.y << ',' << visible.chunkPos.z << " carries binding " << visible.binding.vertexOffset << ", expected " << expected->second << ')';
            return;
        }
    }

    std::sort(actual.begin(), actual.end());
    if (std::adjacent_find(actual.begin(), actual.end()) != actual.end()) {
        ADD_FAILURE() << what << " (a chunk was emitted more than once)";
        return;
    }

    const std::vector<ChunkKey> expected = referenceVisible(source, frustum, cameraPosition);
    if (actual != expected) {
        ADD_FAILURE() << what << " (visible " << actual.size() << ", reference " << expected.size() << ')';
    }
}

Frustum halfSpaceFrustum() {
    Frustum frustum = uniformFrustum(1.0f);
    frustum.planes[0] = glm::vec4(1.0f, 0.0f, 0.0f, -1.0f);
    return frustum;
}

TEST(ChunkCullerTest, FrustumPlanes) {
    const Frustum slab = halfSpaceFrustum();
    check(chunkInFrustum(slab, glm::ivec3(0, 0, 0)), "a box straddling the plane is inside");
    check(chunkInFrustum(slab, glm::ivec3(4, -3, 7)), "a box well inside is inside");
    check(!chunkInFrustum(slab, glm::ivec3(-1, 0, 0)), "the box ending on the plane is rejected");
    check(!chunkInFrustum(slab, glm::ivec3(-9, 2, 2)), "a box far outside is rejected");

    check(!chunkInFrustum(uniformFrustum(-1.0f), glm::ivec3(0, 0, 0)), "a closed frustum rejects everything");
}

TEST(ChunkCullerTest, ConnectivityCulling) {
    const Frustum open = uniformFrustum(1.0f);
    const Frustum closed = uniformFrustum(-1.0f);
    const Frustum halfSpace = halfSpaceFrustum();

    ChunkCuller culler;

    {
        ChunkList empty;
        culler.cull(empty.view(), open, glm::vec3(0.0f));
        check(culler.visibleChunkIndices().empty(), "nothing loaded draws nothing");
    }

    {
        ChunkList source;
        source.add(glm::ivec3(0, 0, 0), {});
        source.add(glm::ivec3(4000, 0, 4000), {});
        compareCull(culler, source, open, cameraInChunk(glm::ivec3(0, 0, 0)), "bounding box over the cell cap");
        check(culler.visibleChunkIndices().size() == 2, "the oversized box falls back to the frustum test");
        compareCull(culler, source, halfSpace, cameraInChunk(glm::ivec3(0, 0, 0)), "oversized box, half space");

        source.removeLast();
        source.add(glm::ivec3(2, 0, 0), {});
        compareCull(culler, source, open, cameraInChunk(glm::ivec3(0, 0, 0)), "back under the cell cap");
        check(culler.visibleChunkIndices().size() == 1, "a small box uses the fill again");
    }

    {
        ChunkList source;
        for (int y = -1; y <= 1; ++y)
            for (int z = -1; z <= 1; ++z)
                for (int x = -1; x <= 1; ++x)
                    source.add(glm::ivec3(x, y, z), kOpenChunkFaceConnectivity);
        compareCull(culler, source, open, cameraInChunk(glm::ivec3(0)), "open 3x3x3");
        check(culler.visibleChunkIndices().size() == 27, "an open 3x3x3 is fully visible");
        check(!culler.visibleChunkIndices().empty() && source.chunks[culler.visibleChunkIndices()[0]].chunkPos == glm::ivec3(0), "the camera's chunk comes out first");
        compareCull(culler, source, closed, cameraInChunk(glm::ivec3(0)), "open 3x3x3, closed frustum");
        check(culler.visibleChunkIndices().empty(), "a closed frustum leaves nothing to draw");
        compareCull(culler, source, halfSpace, cameraInChunk(glm::ivec3(0)), "open 3x3x3, half space");
        check(culler.visibleChunkIndices().size() == 18, "the half space keeps two of three chunk columns");
    }

    {
        ChunkList source;
        for (int y = -1; y <= 1; ++y)
            for (int z = -1; z <= 1; ++z)
                for (int x = -1; x <= 1; ++x)
                    source.add(glm::ivec3(x, y, z), {});
        compareCull(culler, source, open, cameraInChunk(glm::ivec3(0)), "sealed 3x3x3");
        check(culler.visibleChunkIndices().size() == 7, "sealed neighbours are drawn but nothing behind them");
    }

    {
        ChunkList source;
        for (int x = 0; x < 8; ++x) {
            source.add(glm::ivec3(x, 0, 0), pairConnectivity(0, 1));
        }
        source.add(glm::ivec3(4, 1, 0), kOpenChunkFaceConnectivity);
        source.add(glm::ivec3(20, 0, 0), kOpenChunkFaceConnectivity);
        compareCull(culler, source, open, cameraInChunk(glm::ivec3(0, 0, 0)), "x corridor");
        check(culler.visibleChunkIndices().size() == 8, "the corridor is drawn, the side chunk and the island are not");

        compareCull(culler, source, open, cameraInChunk(glm::ivec3(4, 0, 0)), "x corridor, mid camera");
        check(culler.visibleChunkIndices().size() == 9, "a mid-corridor camera also sees the side chunk");
    }

    {
        ChunkList source;
        for (int x = 0; x < 4; ++x) {
            source.add(glm::ivec3(x, 0, 0), {});
        }
        compareCull(culler, source, open, cameraInChunk(glm::ivec3(0, 8, 0)), "camera outside the loaded set");
        check(culler.visibleChunkIndices().size() == 4, "the fallback draws everything loaded");
        compareCull(culler, source, halfSpace, cameraInChunk(glm::ivec3(-40, 0, 0)), "fallback, half space");
        check(culler.visibleChunkIndices().size() == 4, "the fallback still honours the frustum");
        compareCull(culler, source, halfSpace, cameraInChunk(glm::ivec3(0, 0, 40)), "fallback from a third direction");
    }

    {
        Rng rng{0x5EEDFACE1234ull};
        const glm::ivec3 origins[] = {{0, 0, 0}, {5, 1, 5}, {-7, -2, 3}, {30, 0, 30}, {-40, 1, -9}, {0, -3, 0}};
        for (int pass = 0; pass < 600; ++pass) {
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
            compareCull(culler, source, rng.below(2) == 0 ? open : halfSpace, cameraInChunk(cameraChunk), "random chunk set");
        }
    }
}

using SCM = ServerChunkManager;

constexpr int kDemandWorld = 5;
constexpr int kDemandRadius = 1;
constexpr long kUnloadDelayTicks = 3;

glm::ivec3 toVec(ChunkKey key) {
    return glm::ivec3(key[0], key[1], key[2]);
}

bool inDemandWorld(glm::ivec3 pos) {
    return pos.x >= 0 && pos.x < kDemandWorld && pos.y >= 0 && pos.y < kDemandWorld &&
           pos.z >= 0 && pos.z < kDemandWorld;
}

struct DemandRequester {
    SCM::PriorityClass priorityClass = SCM::PriorityClass::Player;
    glm::ivec3 focus{0, 0, 0};
    std::vector<ChunkKey> visible;
    std::vector<ChunkKey> retention;

    void rebuild() {
        visible.clear();
        retention.clear();
        for (int dx = -kDemandRadius - 1; dx <= kDemandRadius + 1; ++dx) {
            for (int dy = -kDemandRadius - 1; dy <= kDemandRadius + 1; ++dy) {
                for (int dz = -kDemandRadius - 1; dz <= kDemandRadius + 1; ++dz) {
                    const glm::ivec3 pos = focus + glm::ivec3(dx, dy, dz);
                    if (!inDemandWorld(pos)) {
                        continue;
                    }
                    const int ring = std::max(std::abs(dx), std::max(std::abs(dy), std::abs(dz)));
                    (ring <= kDemandRadius ? visible : retention).push_back(keyOf(pos));
                }
            }
        }
        std::sort(visible.begin(), visible.end());
        std::sort(retention.begin(), retention.end());
    }
};

constexpr size_t kClassCount = static_cast<size_t>(SCM::PriorityClass::Count);

struct DemandCounts {
    std::array<uint32_t, kClassCount> perClass{};
    uint32_t requesterCount = 0;
    uint32_t retentionCount = 0;
};

struct RefManager {
    struct Entry {
        SCM::State state = SCM::State::Unloaded;
        std::array<uint32_t, kClassCount> perClass{};
        uint32_t requesterCount = 0;
        uint32_t retentionCount = 0;
        uint64_t generationId = 0;
        long unloadTick = LONG_MAX;

        SCM::PriorityClass priorityClass() const {
            for (size_t index = 0; index < kClassCount; ++index) {
                if (perClass[index] > 0) {
                    return static_cast<SCM::PriorityClass>(index);
                }
            }
            return static_cast<SCM::PriorityClass>(kClassCount - 1);
        }
    };

    std::map<ChunkKey, Entry> entries;
    uint64_t nextGenerationId = 1;

    void updateDemands(const std::map<ChunkKey, DemandCounts>& demands, const std::set<ChunkKey>& regained, long tick) {
        for (const auto& [key, counts] : demands) {
            entries.try_emplace(key);
        }
        for (auto& [key, entry] : entries) {
            const auto it = demands.find(key);
            entry.perClass = it == demands.end() ? std::array<uint32_t, kClassCount>{} : it->second.perClass;
            entry.requesterCount = it == demands.end() ? 0 : it->second.requesterCount;
            entry.retentionCount = it == demands.end() ? 0 : it->second.retentionCount;

            switch (entry.state) {
                case SCM::State::Unloaded:
                    if (entry.requesterCount > 0) {
                        entry.state = SCM::State::Queued;
                    }
                    break;
                case SCM::State::Queued:
                    if (entry.requesterCount == 0) {
                        entry.state = SCM::State::Unloaded;
                    }
                    break;
                case SCM::State::Generating:
                    break;
                case SCM::State::ReadyToCommit:
                    if (entry.requesterCount == 0) {
                        entry.state = SCM::State::Unloaded;
                    }
                    break;
                case SCM::State::Loaded:
                    if (entry.requesterCount == 0 && entry.retentionCount == 0) {
                        entry.state = SCM::State::UnloadPending;
                        entry.unloadTick = tick + kUnloadDelayTicks;
                    }
                    break;
                case SCM::State::UnloadPending:
                    if (entry.requesterCount > 0 || entry.retentionCount > 0) {
                        entry.state = SCM::State::Loaded;
                        entry.unloadTick = LONG_MAX;
                    } else if (regained.count(key) != 0) {
                        entry.unloadTick = tick + kUnloadDelayTicks;
                    }
                    break;
                default:
                    break;
            }
        }
        for (auto it = entries.begin(); it != entries.end();) {
            const Entry& entry = it->second;
            if (entry.state == SCM::State::Unloaded && entry.requesterCount == 0 && entry.retentionCount == 0) {
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::vector<ChunkKey> inState(SCM::State state) const {
        std::vector<ChunkKey> keys;
        for (const auto& [key, entry] : entries) {
            if (entry.state == state) {
                keys.push_back(key);
            }
        }
        return keys;
    }

    size_t countInState(SCM::State state) const { return inState(state).size(); }

    std::vector<glm::ivec3> topQueued(size_t limit, const std::vector<glm::ivec3>& foci) const {
        std::vector<std::pair<std::array<long long, 4>, glm::ivec3>> ordered;
        for (const auto& [key, entry] : entries) {
            if (entry.state != SCM::State::Queued) {
                continue;
            }
            const glm::ivec3 pos = toVec(key);
            long long best = (1LL << 30) - 1;
            for (const glm::ivec3& focus : foci) {
                const glm::ivec3 offset = pos - focus;
                const long long horizontal = offset.x * offset.x + offset.z * offset.z;
                best = std::min(best, horizontal * 1024 + std::abs(offset.y));
            }
            const long long rank = static_cast<long long>(entry.priorityClass()) * (1LL << 30) + best;
            ordered.push_back({{rank, pos.x, pos.y, pos.z}, pos});
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        ordered.resize(std::min(limit, ordered.size()));

        std::vector<glm::ivec3> positions;
        positions.reserve(ordered.size());
        for (const auto& [rank, pos] : ordered) {
            positions.push_back(pos);
        }
        return positions;
    }

    size_t requestedCount() const {
        size_t count = 0;
        for (const auto& [key, entry] : entries) {
            count += entry.requesterCount > 0 ? 1 : 0;
        }
        return count;
    }

    std::vector<ChunkKey> readyToUnload(long tick) const {
        std::vector<ChunkKey> keys;
        for (const auto& [key, entry] : entries) {
            if (entry.state == SCM::State::UnloadPending && entry.requesterCount == 0 &&
                entry.retentionCount == 0 && tick >= entry.unloadTick) {
                keys.push_back(key);
            }
        }
        return keys;
    }

    std::optional<uint64_t> beginGeneration(ChunkKey key) {
        auto it = entries.find(key);
        if (it == entries.end() || it->second.state != SCM::State::Queued || it->second.requesterCount == 0) {
            return std::nullopt;
        }
        it->second.state = SCM::State::Generating;
        it->second.generationId = nextGenerationId++;
        return it->second.generationId;
    }

    bool finishGeneration(ChunkKey key, uint64_t generationId) {
        auto it = entries.find(key);
        if (it == entries.end() || it->second.state != SCM::State::Generating ||
            it->second.generationId != generationId) {
            return false;
        }
        if (it->second.requesterCount == 0) {
            it->second.state = SCM::State::Unloaded;
            return false;
        }
        it->second.state = SCM::State::ReadyToCommit;
        return true;
    }

    bool commitLoaded(ChunkKey key, uint64_t generationId) {
        auto it = entries.find(key);
        if (it == entries.end() || it->second.state != SCM::State::ReadyToCommit ||
            it->second.generationId != generationId || it->second.requesterCount == 0) {
            return false;
        }
        it->second.state = SCM::State::Loaded;
        it->second.unloadTick = LONG_MAX;
        return true;
    }

    void failGeneration(ChunkKey key, uint64_t generationId) {
        auto it = entries.find(key);
        if (it == entries.end() || it->second.generationId != generationId ||
            (it->second.state != SCM::State::Generating && it->second.state != SCM::State::ReadyToCommit)) {
            return;
        }
        it->second.state = it->second.requesterCount > 0 ? SCM::State::Queued : SCM::State::Unloaded;
    }

    bool markUnloaded(ChunkKey key) {
        auto it = entries.find(key);
        if (it == entries.end() || it->second.state != SCM::State::UnloadPending ||
            it->second.requesterCount != 0 || it->second.retentionCount != 0) {
            return false;
        }
        it->second.state = SCM::State::Unloaded;
        it->second.unloadTick = LONG_MAX;
        return true;
    }

    void restoreLoaded(ChunkKey key, long tick) {
        Entry& entry = entries[key];
        if (entry.requesterCount > 0 || entry.retentionCount > 0) {
            entry.state = SCM::State::Loaded;
            entry.unloadTick = LONG_MAX;
        } else {
            entry.state = SCM::State::UnloadPending;
            entry.unloadTick = tick + kUnloadDelayTicks;
        }
    }
};

std::vector<ChunkKey> sortedKeys(const std::vector<glm::ivec3>& positions) {
    std::vector<ChunkKey> keys;
    keys.reserve(positions.size());
    for (const glm::ivec3& pos : positions) {
        keys.push_back(keyOf(pos));
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

TEST(ServerChunkManagerTest, RandomizedStateMachineMatchesReference) {
    Rng rng{0xC0FFEE1234567ull};
    const auto tickTime = [](long tick) {
        return SCM::TimePoint{} + std::chrono::milliseconds(tick * 100);
    };
    SCM manager(std::chrono::milliseconds(kUnloadDelayTicks * 100));
    RefManager ref;

    std::map<int, DemandRequester> requesters;
    int nextRequesterId = 1;
    const SCM::PriorityClass classes[] = {SCM::PriorityClass::LoadingCore, SCM::PriorityClass::Player, SCM::PriorityClass::Robot};

    std::set<ChunkKey> regained;
    const auto applyAdd = [&](const DemandRequester& requester) {
        for (const ChunkKey& key : requester.visible) {
            manager.addRequester(toVec(key), requester.priorityClass);
            regained.insert(key);
        }
        for (const ChunkKey& key : requester.retention) {
            manager.addRetention(toVec(key));
            regained.insert(key);
        }
    };
    const auto applyRemove = [&](const DemandRequester& requester, long tick) {
        for (const ChunkKey& key : requester.visible) {
            manager.removeRequester(toVec(key), requester.priorityClass, tickTime(tick));
        }
        for (const ChunkKey& key : requester.retention) {
            manager.removeRetention(toVec(key), tickTime(tick));
        }
    };

    for (long tick = 0; tick < 12000; ++tick) {
        regained.clear();

        if (!requesters.empty() && rng.below(100) < 6) {
            const auto victim = std::next(requesters.begin(), rng.below(static_cast<uint32_t>(requesters.size())));
            applyRemove(victim->second, tick);
            requesters.erase(victim);
        }
        if (requesters.size() < 4 && rng.below(100) < 20) {
            DemandRequester requester;
            requester.priorityClass = classes[rng.below(3)];
            requester.focus = glm::ivec3(rng.below(kDemandWorld), rng.below(kDemandWorld), rng.below(kDemandWorld));
            requester.rebuild();
            applyAdd(requester);
            requesters.emplace(nextRequesterId++, std::move(requester));
        }
        if (!requesters.empty() && rng.below(100) < 45) {
            auto mover = std::next(requesters.begin(), rng.below(static_cast<uint32_t>(requesters.size())));
            DemandRequester moved = mover->second;
            moved.focus = glm::ivec3(rng.below(kDemandWorld), rng.below(kDemandWorld), rng.below(kDemandWorld));
            moved.rebuild();
            if (moved.focus != mover->second.focus) {
                applyAdd(moved);
                applyRemove(mover->second, tick);
                mover->second = std::move(moved);
            }
        }

        std::map<ChunkKey, DemandCounts> demands;
        std::vector<glm::ivec3> foci;
        for (const auto& [id, requester] : requesters) {
            foci.push_back(requester.focus);
            for (const ChunkKey& key : requester.visible) {
                DemandCounts& counts = demands[key];
                ++counts.perClass[static_cast<size_t>(requester.priorityClass)];
                ++counts.requesterCount;
            }
            for (const ChunkKey& key : requester.retention) {
                ++demands[key].retentionCount;
            }
        }
        ref.updateDemands(demands, regained, tick);

        if (manager.requestedChunkCount() != ref.requestedCount()) {
            check(false, "requested chunk count diverged");
            break;
        }
        bool stateCountsMatch = true;
        for (const SCM::State state : {SCM::State::Queued, SCM::State::Generating, SCM::State::ReadyToCommit,
                                       SCM::State::Loaded, SCM::State::UnloadPending}) {
            stateCountsMatch = stateCountsMatch && manager.stateCount(state) == ref.countInState(state);
        }
        if (!stateCountsMatch) {
            check(false, "state counts diverged");
            break;
        }

        if (sortedKeys(manager.queuedChunks(1024, foci)) != ref.inState(SCM::State::Queued)) {
            check(false, "queued set diverged");
            break;
        }

        const size_t limit = 1 + rng.below(8);
        const std::vector<glm::ivec3> queued = manager.queuedChunks(limit, foci);
        if (queued != ref.topQueued(limit, foci)) {
            check(false, "queued order diverged");
            break;
        }

        if (rng.below(4) != 0) {
            const size_t take = std::min<size_t>(queued.size(), 1 + rng.below(6));
            for (size_t i = 0; i < take; ++i) {
                const glm::ivec3 pos = queued[i];
                const ChunkKey key = keyOf(pos);
                const std::optional<uint64_t> generationId = manager.beginGeneration(pos);
                const std::optional<uint64_t> refGenerationId = ref.beginGeneration(key);
                check(generationId.has_value() == refGenerationId.has_value(), "beginGeneration agrees");
                if (!generationId || !refGenerationId) {
                    continue;
                }
                if (rng.below(12) == 0) {
                    manager.failGeneration(pos, *generationId);
                    ref.failGeneration(key, *refGenerationId);
                    continue;
                }
                const bool finished = manager.finishGeneration(pos, *generationId);
                check(finished == ref.finishGeneration(key, *refGenerationId), "finishGeneration agrees");
                if (!finished) {
                    continue;
                }
                const bool committed = manager.commitLoaded(pos, *generationId);
                check(committed == ref.commitLoaded(key, *refGenerationId), "commitLoaded agrees");
                if (!committed) {
                    manager.failGeneration(pos, *generationId);
                    ref.failGeneration(key, *refGenerationId);
                }
            }
        }

        const std::vector<glm::ivec3> readyToUnload = manager.chunksReadyToUnload(tickTime(tick));
        if (sortedKeys(readyToUnload) != ref.readyToUnload(tick)) {
            check(false, "chunksReadyToUnload diverged");
            break;
        }
        for (const glm::ivec3& pos : readyToUnload) {
            const ChunkKey key = keyOf(pos);
            const bool marked = manager.markUnloaded(pos);
            check(marked == ref.markUnloaded(key), "markUnloaded agrees");
            if (marked && rng.below(10) == 0) {
                manager.restoreLoaded(pos, tickTime(tick));
                ref.restoreLoaded(key, tick);
            }
        }

        if (manager.trackedChunkCount() < ref.entries.size()) {
            check(false, "the incremental manager dropped an entry the model still holds");
            break;
        }
    }

    long tick = 12000;
    for (const auto& [id, requester] : requesters) {
        applyRemove(requester, tick);
    }
    requesters.clear();
    for (; tick < 12010; ++tick) {
        manager.queuedChunks(1024, {});
        for (const glm::ivec3& pos : manager.chunksReadyToUnload(tickTime(tick))) {
            check(manager.markUnloaded(pos), "the final unload sweep marks every expired chunk");
        }
    }
    manager.queuedChunks(1024, {});
    manager.chunksReadyToUnload(tickTime(tick));
    check(manager.requestedChunkCount() == 0, "no requester reference survives");
    if (manager.trackedChunkCount() != 0) {
        ADD_FAILURE() << manager.trackedChunkCount() << " entries leaked after every requester left";
    }
}

}  // namespace
