#include "chunk_mesh.h"

#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <vector>

#include "block.h"
#include "chunk.h"
#include "helper.h"
#include "profiler.h"
#include "voxel_world.h"

namespace {

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

const glm::vec3 kLightDirection = glm::normalize(glm::vec3(0.4f, 1.0f, 0.55f));
constexpr float kAmbient = 0.3f;

struct Face {
    glm::ivec3 normal;
    std::array<glm::ivec3, 4> corners;
};

constexpr std::array<Face, 6> kFaces = {{
    {glm::ivec3(1, 0, 0), {glm::ivec3(1, 0, 0), glm::ivec3(1, 1, 0), glm::ivec3(1, 1, 1), glm::ivec3(1, 0, 1)}},
    {glm::ivec3(-1, 0, 0), {glm::ivec3(0, 0, 1), glm::ivec3(0, 1, 1), glm::ivec3(0, 1, 0), glm::ivec3(0, 0, 0)}},
    {glm::ivec3(0, 1, 0), {glm::ivec3(0, 1, 1), glm::ivec3(1, 1, 1), glm::ivec3(1, 1, 0), glm::ivec3(0, 1, 0)}},
    {glm::ivec3(0, -1, 0), {glm::ivec3(0, 0, 0), glm::ivec3(1, 0, 0), glm::ivec3(1, 0, 1), glm::ivec3(0, 0, 1)}},
    {glm::ivec3(0, 0, 1), {glm::ivec3(1, 0, 1), glm::ivec3(1, 1, 1), glm::ivec3(0, 1, 1), glm::ivec3(0, 0, 1)}},
    {glm::ivec3(0, 0, -1), {glm::ivec3(0, 0, 0), glm::ivec3(0, 1, 0), glm::ivec3(1, 1, 0), glm::ivec3(1, 0, 0)}},
}};

constexpr bool facesMatchChunkFaceOrder() {
    for (size_t face = 0; face < kFaces.size(); ++face) {
        const glm::ivec3 expected = kChunkFaceOffsets[face];
        if (kFaces[face].normal.x != expected.x || kFaces[face].normal.y != expected.y || kFaces[face].normal.z != expected.z) {
            return false;
        }
    }
    return true;
}
static_assert(facesMatchChunkFaceOrder(), "buildFaceMasks hard-codes the axis per face index");

constexpr int bitIndex(int faceA, int faceB) {
    return faceA < faceB ? faceA * 6 + faceB : faceB * 6 + faceA;
}
static_assert(bitIndex(4, 5) < static_cast<int>(sizeof(ChunkFaceConnectivity) * 8));

constexpr size_t kRowCount = ChunkLayout::BLOCK_COUNT / ChunkLayout::SIZE;
using RowMasks = std::array<uint16_t, kRowCount>;
static_assert(ChunkLayout::SIZE == 16, "Rows are held in a uint16_t bitmask");

constexpr size_t rowIndex(int y, int z) {
    return (static_cast<size_t>(y) << ChunkLayout::SIZE_BITS) | static_cast<size_t>(z);
}
static_assert(rowIndex(1, 2) * ChunkLayout::SIZE == ChunkLayout::blockIndex({0, 1, 2}),
              "A (y, z) row must be ChunkLayout::SIZE consecutive block indices starting at rowIndex * SIZE");

constexpr size_t kRowStrideY = ChunkLayout::SIZE;
static_assert(rowIndex(2, 3) - rowIndex(1, 3) == kRowStrideY);
static_assert(rowIndex(1, 4) - rowIndex(1, 3) == 1);

uint16_t uniformAirRow(BlockData block) {
    return block.type == BlockType::Air ? uint16_t{0xFFFF} : uint16_t{0};
}

uint16_t airRowOf(const Chunk& chunk, int y, int z) {
    const size_t base = rowIndex(y, z) * ChunkLayout::SIZE;
    uint16_t mask = 0;
    for (int x = 0; x < ChunkLayout::SIZE; ++x) {
        if (chunk.getBlock(base + static_cast<size_t>(x)).type == BlockType::Air) {
            mask = static_cast<uint16_t>(mask | (1u << x));
        }
    }
    return mask;
}

RowMasks buildAirRows(const Chunk& chunk) {
    RowMasks air{};
    if (chunk.isUniform()) {
        air.fill(uniformAirRow(chunk.uniformBlock()));
        return air;
    }
    for (int y = 0; y < ChunkLayout::SIZE; ++y) {
        for (int z = 0; z < ChunkLayout::SIZE; ++z) {
            air[rowIndex(y, z)] = airRowOf(chunk, y, z);
        }
    }
    return air;
}

uint16_t neighborAirRow(const Chunk* neighbor, int y, int z) {
    if (neighbor == nullptr) {
        return uint16_t{0xFFFF};
    }
    if (neighbor->isUniform()) {
        return uniformAirRow(neighbor->uniformBlock());
    }
    return airRowOf(*neighbor, y, z);
}

bool neighborAirAt(const Chunk* neighbor, glm::ivec3 localPos) {
    if (neighbor == nullptr) {
        return true;
    }
    if (neighbor->isUniform()) {
        return neighbor->uniformBlock().type == BlockType::Air;
    }
    return neighbor->getBlock(ChunkLayout::blockIndex(localPos)).type == BlockType::Air;
}

using FaceMasks = std::array<RowMasks, 6>;

FaceMasks buildFaceMasks(const RowMasks& air, const std::array<const Chunk*, 6>& neighbors) {
    constexpr int last = ChunkLayout::SIZE - 1;
    FaceMasks masks{};

    for (int y = 0; y < ChunkLayout::SIZE; ++y) {
        for (int z = 0; z < ChunkLayout::SIZE; ++z) {
            const size_t row = rowIndex(y, z);
            const auto solid = static_cast<uint16_t>(~air[row]);
            if (solid == 0) {
                continue;
            }

            uint16_t across[6];
            across[0] = static_cast<uint16_t>(air[row] >> 1);
            if ((solid & 0x8000u) != 0 && neighborAirAt(neighbors[0], {0, y, z})) {
                across[0] = static_cast<uint16_t>(across[0] | 0x8000u);
            }
            across[1] = static_cast<uint16_t>(air[row] << 1);
            if ((solid & 0x0001u) != 0 && neighborAirAt(neighbors[1], {last, y, z})) {
                across[1] = static_cast<uint16_t>(across[1] | 0x0001u);
            }
            across[2] = y < last ? air[row + kRowStrideY] : neighborAirRow(neighbors[2], 0, z);
            across[3] = y > 0 ? air[row - kRowStrideY] : neighborAirRow(neighbors[3], last, z);
            across[4] = z < last ? air[row + 1] : neighborAirRow(neighbors[4], y, 0);
            across[5] = z > 0 ? air[row - 1] : neighborAirRow(neighbors[5], y, last);

            for (size_t face = 0; face < kFaces.size(); ++face) {
                masks[face][row] = static_cast<uint16_t>(solid & across[face]);
            }
        }
    }
    return masks;
}

uint16_t spreadAlongX(uint16_t reached, uint16_t air) {
    for (;;) {
        const uint16_t next = static_cast<uint16_t>(reached | (reached << 1) | (reached >> 1)) & air;
        if (next == reached) {
            return reached;
        }
        reached = next;
    }
}

uint8_t touchedFaces(size_t row, uint16_t bits) {
    const int y = static_cast<int>(row >> ChunkLayout::SIZE_BITS);
    const int z = static_cast<int>(row & (ChunkLayout::SIZE - 1));
    unsigned touched = 0;
    if (bits & 0x8000u) touched |= 1u << 0;
    if (bits & 0x0001u) touched |= 1u << 1;
    if (y == ChunkLayout::SIZE - 1) touched |= 1u << 2;
    if (y == 0) touched |= 1u << 3;
    if (z == ChunkLayout::SIZE - 1) touched |= 1u << 4;
    if (z == 0) touched |= 1u << 5;
    return static_cast<uint8_t>(touched);
}

ChunkFaceConnectivity computeFaceConnectivity(const RowMasks& air) {
    RowMasks visited{};
    std::array<uint16_t, kRowCount> stack{};
    std::array<uint8_t, kRowCount> queued{};
    ChunkFaceConnectivity mask = 0;

    for (size_t seedRow = 0; seedRow < kRowCount; ++seedRow) {
        for (;;) {
            const auto seedBits = static_cast<uint16_t>(air[seedRow] & ~visited[seedRow]);
            if (seedBits == 0) {
                break;
            }

            unsigned touched = 0;
            size_t stackSize = 0;
            const auto offer = [&](size_t row, uint16_t bits) {
                const auto added = static_cast<uint16_t>(bits & air[row] & ~visited[row]);
                if (added == 0) {
                    return;
                }
                visited[row] = static_cast<uint16_t>(visited[row] | added);
                touched |= touchedFaces(row, added);
                if (queued[row] == 0) {
                    queued[row] = 1;
                    stack[stackSize++] = static_cast<uint16_t>(row);
                }
            };

            offer(seedRow, static_cast<uint16_t>(1u << std::countr_zero(seedBits)));
            while (stackSize > 0) {
                const size_t row = stack[--stackSize];
                queued[row] = 0;

                const uint16_t before = visited[row];
                const uint16_t reached = spreadAlongX(before, air[row]);
                if (reached != before) {
                    visited[row] = reached;
                    touched |= touchedFaces(row, static_cast<uint16_t>(reached & ~before));
                }

                const int y = static_cast<int>(row >> ChunkLayout::SIZE_BITS);
                const int z = static_cast<int>(row & (ChunkLayout::SIZE - 1));
                if (y > 0) offer(row - kRowStrideY, reached);
                if (y < ChunkLayout::SIZE - 1) offer(row + kRowStrideY, reached);
                if (z > 0) offer(row - 1, reached);
                if (z < ChunkLayout::SIZE - 1) offer(row + 1, reached);
            }

            for (int from = 0; from < 6; ++from) {
                if ((touched & (1u << from)) == 0) {
                    continue;
                }
                for (int to = from + 1; to < 6; ++to) {
                    if ((touched & (1u << to)) != 0) {
                        mask |= ChunkFaceConnectivity{1} << bitIndex(from, to);
                    }
                }
            }
        }
    }
    return mask;
}

using ShadedColorTable = std::array<std::array<uint32_t, 6>, static_cast<size_t>(BlockType::Count)>;

const ShadedColorTable& shadedColorTable() {
    static const ShadedColorTable table = [] {
        ShadedColorTable built{};
        for (size_t type = 0; type < built.size(); ++type) {
            for (size_t face = 0; face < kFaces.size(); ++face) {
                const float wrapped = glm::dot(glm::vec3(kFaces[face].normal), kLightDirection) * 0.5f + 0.5f;
                const float shade = kAmbient + (1.0f - kAmbient) * wrapped;
                built[type][face] = packColor(kBlockAlbedo[type] * shade);
            }
        }
        return built;
    }();
    return table;
}

void appendFace(std::vector<ChunkVertex>& vertices, glm::ivec3 localPos, size_t faceIndex, uint32_t color) {
    for (const glm::ivec3& corner : kFaces[faceIndex].corners) {
        const glm::ivec3 vertexPos = localPos + corner;
        vertices.push_back(ChunkVertex{
            static_cast<uint8_t>(vertexPos.x),
            static_cast<uint8_t>(vertexPos.y),
            static_cast<uint8_t>(vertexPos.z),
            0,
            color,
        });
    }
}

}  // namespace

bool chunkFacesConnected(ChunkFaceConnectivity mask, int faceA, int faceB) {
    if (faceA == faceB) {
        return false;
    }
    return (mask & (ChunkFaceConnectivity{1} << bitIndex(faceA, faceB))) != 0;
}

void buildChunkMesh(const VoxelWorld& voxelWorld, glm::ivec3 chunkPos, ChunkMesh& out) {
    out.vertices.clear();
    out.faceConnectivity = ~0u;

    const Chunk* chunkPtr = voxelWorld.findChunk(chunkPos);
    if (chunkPtr == nullptr) {
        return;
    }
    const Chunk& chunk = *chunkPtr;
    if (chunk.isEmpty()) {
        return;
    }

    std::array<const Chunk*, 6> neighbors{};
    for (size_t face = 0; face < neighbors.size(); ++face) {
        neighbors[face] = voxelWorld.findChunk(chunkPos + kChunkFaceOffsets[face]);
    }

    const RowMasks air = buildAirRows(chunk);
    const FaceMasks faceMasks = buildFaceMasks(air, neighbors);

    size_t faceCount = 0;
    for (const RowMasks& rows : faceMasks) {
        for (uint16_t mask : rows) {
            faceCount += static_cast<size_t>(std::popcount(mask));
        }
    }
    out.vertices.reserve(faceCount * 4);
    assert(faceCount * 4 <= kMaxChunkMeshVertices);

    const ShadedColorTable& colorTable = shadedColorTable();
    for (int y = 0; y < ChunkLayout::SIZE; ++y) {
        for (int z = 0; z < ChunkLayout::SIZE; ++z) {
            const size_t row = rowIndex(y, z);
            uint16_t pending = 0;
            for (const RowMasks& rows : faceMasks) {
                pending = static_cast<uint16_t>(pending | rows[row]);
            }

            while (pending != 0) {
                const int x = std::countr_zero(pending);
                pending = static_cast<uint16_t>(pending & (pending - 1));

                const auto bit = static_cast<uint16_t>(1u << x);
                const BlockData block = chunk.getBlock(row * ChunkLayout::SIZE + static_cast<size_t>(x));
                assert(block.type < BlockType::Count);
                const std::array<uint32_t, 6>& faceColors = colorTable[static_cast<size_t>(block.type)];
                for (size_t face = 0; face < faceMasks.size(); ++face) {
                    if ((faceMasks[face][row] & bit) != 0) {
                        appendFace(out.vertices, glm::ivec3(x, y, z), face, faceColors[face]);
                    }
                }
            }
        }
    }

    out.faceConnectivity = computeFaceConnectivity(air);
    MW_PROFILE_COUNTER("Client.ChunkMeshVertices", static_cast<int64_t>(out.vertices.size()));
}
