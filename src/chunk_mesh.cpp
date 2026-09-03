#include "chunk_mesh.h"

#include <array>
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

struct NeighborStep {
    int indexOffset;
    int axis;
    int boundary;
};

constexpr std::array<NeighborStep, 6> kNeighborSteps = {{
    {ChunkLayout::X_STRIDE, 0, ChunkLayout::SIZE - 1},
    {-ChunkLayout::X_STRIDE, 0, 0},
    {ChunkLayout::Y_STRIDE, 1, ChunkLayout::SIZE - 1},
    {-ChunkLayout::Y_STRIDE, 1, 0},
    {ChunkLayout::Z_STRIDE, 2, ChunkLayout::SIZE - 1},
    {-ChunkLayout::Z_STRIDE, 2, 0},
}};

constexpr int indexOffsetForNormal(glm::ivec3 normal) {
    return normal.x * ChunkLayout::X_STRIDE + normal.y * ChunkLayout::Y_STRIDE + normal.z * ChunkLayout::Z_STRIDE;
}

constexpr bool neighborStepsMatchFaces() {
    for (size_t face = 0; face < kFaces.size(); ++face) {
        if (kNeighborSteps[face].indexOffset != indexOffsetForNormal(kFaces[face].normal)) {
            return false;
        }
    }
    return true;
}
static_assert(neighborStepsMatchFaces(), "kNeighborSteps must stay aligned with kFaces");

constexpr int bitIndex(int faceA, int faceB) {
    return faceA < faceB ? faceA * 6 + faceB : faceB * 6 + faceA;
}
static_assert(bitIndex(4, 5) < static_cast<int>(sizeof(ChunkFaceConnectivity) * 8));

struct ChunkNeighborhood {
    const Chunk* center = nullptr;
    std::array<const Chunk*, 6> neighbors{};

    BlockData blockAcross(size_t index, glm::ivec3 localPos, size_t faceIndex) const {
        const NeighborStep& step = kNeighborSteps[faceIndex];
        if (localPos[step.axis] != step.boundary) {
            return center->getBlock(static_cast<size_t>(static_cast<int>(index) + step.indexOffset));
        }
        const Chunk* neighbor = neighbors[faceIndex];
        if (neighbor == nullptr) {
            return BlockData{};
        }
        glm::ivec3 acrossPos = localPos;
        acrossPos[step.axis] = step.boundary == 0 ? ChunkLayout::SIZE - 1 : 0;
        return neighbor->getBlock(ChunkLayout::blockIndex(acrossPos));
    }
};

ChunkNeighborhood buildNeighborhood(const VoxelWorld& voxelWorld, const Chunk& chunk) {
    ChunkNeighborhood neighborhood;
    neighborhood.center = &chunk;
    const glm::ivec3 chunkPos = chunk.getPosition();
    for (size_t faceIndex = 0; faceIndex < kFaces.size(); ++faceIndex) {
        neighborhood.neighbors[faceIndex] = voxelWorld.findChunk(chunkPos + kFaces[faceIndex].normal);
    }
    return neighborhood;
}

constexpr size_t kRowCount = ChunkLayout::BLOCK_COUNT / ChunkLayout::SIZE;
using AirRows = std::array<uint16_t, kRowCount>;
static_assert(ChunkLayout::SIZE == 16, "Rows are held in a uint16_t bitmask");

constexpr size_t rowIndex(int y, int z) {
    return (static_cast<size_t>(y) << ChunkLayout::SIZE_BITS) | static_cast<size_t>(z);
}
static_assert(rowIndex(1, 2) * ChunkLayout::SIZE == ChunkLayout::blockIndex({0, 1, 2}),
              "A (y, z) row must be ChunkLayout::SIZE consecutive block indices starting at rowIndex * SIZE");

AirRows buildAirRows(const Chunk& chunk) {
    AirRows air{};
    for (size_t row = 0; row < kRowCount; ++row) {
        const size_t base = row * ChunkLayout::SIZE;
        uint16_t mask = 0;
        for (int x = 0; x < ChunkLayout::SIZE; ++x) {
            if (chunk.getBlock(base + static_cast<size_t>(x)).type == BlockType::Air) {
                mask |= static_cast<uint16_t>(1u << x);
            }
        }
        air[row] = mask;
    }
    return air;
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

ChunkFaceConnectivity computeFaceConnectivity(const Chunk& chunk) {
    const AirRows air = buildAirRows(chunk);

    uint8_t reachable[6] = {};
    AirRows visited{};
    std::array<uint8_t, kRowCount> queued{};
    std::vector<uint16_t> stack;
    stack.reserve(kRowCount);

    auto offer = [&](size_t row, uint16_t bits) {
        const uint16_t added = static_cast<uint16_t>(bits & air[row] & ~visited[row]);
        if (added == 0) {
            return;
        }
        visited[row] |= added;
        if (!queued[row]) {
            queued[row] = 1;
            stack.push_back(static_cast<uint16_t>(row));
        }
    };

    for (int face = 0; face < 6; ++face) {
        visited.fill(0);
        queued.fill(0);
        stack.clear();

        for (int a = 0; a < ChunkLayout::SIZE; ++a) {
            switch (face) {
                case 0:
                    for (int z = 0; z < ChunkLayout::SIZE; ++z) offer(rowIndex(a, z), 0x8000u);
                    break;
                case 1:
                    for (int z = 0; z < ChunkLayout::SIZE; ++z) offer(rowIndex(a, z), 0x0001u);
                    break;
                case 2: offer(rowIndex(ChunkLayout::SIZE - 1, a), 0xFFFFu); break;
                case 3: offer(rowIndex(0, a), 0xFFFFu); break;
                case 4: offer(rowIndex(a, ChunkLayout::SIZE - 1), 0xFFFFu); break;
                case 5: offer(rowIndex(a, 0), 0xFFFFu); break;
            }
        }

        while (!stack.empty()) {
            const size_t row = stack.back();
            stack.pop_back();
            queued[row] = 0;

            const uint16_t bits = spreadAlongX(visited[row], air[row]);
            visited[row] = bits;

            const int y = static_cast<int>(row >> ChunkLayout::SIZE_BITS);
            const int z = static_cast<int>(row & (ChunkLayout::SIZE - 1));
            if (y > 0) offer(row - ChunkLayout::SIZE, bits);
            if (y < ChunkLayout::SIZE - 1) offer(row + ChunkLayout::SIZE, bits);
            if (z > 0) offer(row - 1, bits);
            if (z < ChunkLayout::SIZE - 1) offer(row + 1, bits);
        }

        uint16_t anyRow = 0;
        uint8_t touched = 0;
        for (size_t row = 0; row < kRowCount; ++row) {
            if (visited[row] == 0) {
                continue;
            }
            anyRow |= visited[row];
            const int y = static_cast<int>(row >> ChunkLayout::SIZE_BITS);
            const int z = static_cast<int>(row & (ChunkLayout::SIZE - 1));
            if (y == ChunkLayout::SIZE - 1) touched |= 1u << 2;
            if (y == 0) touched |= 1u << 3;
            if (z == ChunkLayout::SIZE - 1) touched |= 1u << 4;
            if (z == 0) touched |= 1u << 5;
        }
        if (anyRow & 0x8000u) touched |= 1u << 0;
        if (anyRow & 0x0001u) touched |= 1u << 1;
        reachable[face] = touched;
    }

    ChunkFaceConnectivity mask = 0;
    for (int from = 0; from < 6; ++from)
        for (int to = from + 1; to < 6; ++to)
            if ((reachable[from] >> to) & 1)
                mask |= ChunkFaceConnectivity{1} << bitIndex(from, to);
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

void emitFace(ChunkMesh& mesh, glm::ivec3 localPos, size_t faceIndex, uint32_t color) {
    assert(mesh.vertices.size() + 4 <= kMaxChunkMeshVertices);
    for (const glm::ivec3& corner : kFaces[faceIndex].corners) {
        const glm::ivec3 vertexPos = localPos + corner;
        mesh.vertices.push_back(ChunkVertex{
            static_cast<uint8_t>(vertexPos.x),
            static_cast<uint8_t>(vertexPos.y),
            static_cast<uint8_t>(vertexPos.z),
            0,
            color,
        });
    }
}

void buildUniformSolidMesh(ChunkMesh& mesh, const ChunkNeighborhood& neighborhood, BlockData block) {
    const std::array<uint32_t, 6>& faceColors = shadedColorTable()[static_cast<size_t>(block.type)];
    for (size_t faceIndex = 0; faceIndex < kFaces.size(); ++faceIndex) {
        const NeighborStep& step = kNeighborSteps[faceIndex];
        const int axisA = (step.axis + 1) % 3;
        const int axisB = (step.axis + 2) % 3;
        for (int a = 0; a < ChunkLayout::SIZE; ++a) {
            for (int b = 0; b < ChunkLayout::SIZE; ++b) {
                glm::ivec3 localPos(0);
                localPos[step.axis] = step.boundary;
                localPos[axisA] = a;
                localPos[axisB] = b;
                const size_t index = ChunkLayout::blockIndex(localPos);
                if (neighborhood.blockAcross(index, localPos, faceIndex).type != BlockType::Air) {
                    continue;
                }
                emitFace(mesh, localPos, faceIndex, faceColors[faceIndex]);
            }
        }
    }
}

}  // namespace

bool chunkFacesConnected(ChunkFaceConnectivity mask, int faceA, int faceB) {
    if (faceA == faceB) {
        return false;
    }
    return (mask & (ChunkFaceConnectivity{1} << bitIndex(faceA, faceB))) != 0;
}

ChunkMesh buildChunkMesh(const VoxelWorld& voxelWorld, glm::ivec3 chunkPos) {
    ChunkMesh mesh;

    const Chunk* chunkPtr = voxelWorld.findChunk(chunkPos);
    if (chunkPtr == nullptr) {
        return mesh;
    }
    const Chunk& chunk = *chunkPtr;

    if (chunk.isEmpty()) {
        mesh.faceConnectivity = ~0u;
        return mesh;
    }

    const ChunkNeighborhood neighborhood = buildNeighborhood(voxelWorld, chunk);

    if (chunk.isUniform()) {
        mesh.vertices.reserve(kFaces.size() * ChunkLayout::SIZE * ChunkLayout::SIZE * 4);
        buildUniformSolidMesh(mesh, neighborhood, chunk.uniformBlock());
        mesh.faceConnectivity = 0;
        MW_PROFILE_COUNTER("Client.ChunkMeshVertices", static_cast<int64_t>(mesh.vertices.size()));
        return mesh;
    }

    const ShadedColorTable& colorTable = shadedColorTable();
    mesh.vertices.reserve(1024);

    for (size_t index = 0; index < ChunkLayout::BLOCK_COUNT; ++index) {
        const BlockData block = chunk.getBlock(index);
        if (block.type == BlockType::Air) {
            continue;
        }
        assert(block.type < BlockType::Count);
        const std::array<uint32_t, 6>& faceColors = colorTable[static_cast<size_t>(block.type)];
        const glm::ivec3 localPos = ChunkLayout::blockPosition(index);

        for (size_t faceIndex = 0; faceIndex < kFaces.size(); ++faceIndex) {
            if (neighborhood.blockAcross(index, localPos, faceIndex).type != BlockType::Air) {
                continue;
            }
            emitFace(mesh, localPos, faceIndex, faceColors[faceIndex]);
        }
    }
    mesh.faceConnectivity = computeFaceConnectivity(chunk);
    MW_PROFILE_COUNTER("Client.ChunkMeshVertices", static_cast<int64_t>(mesh.vertices.size()));
    return mesh;
}
