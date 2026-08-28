#include "chunk_mesh.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <queue>
#include <utility>

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

const std::array<Face, 6> kFaces = {{
    {glm::ivec3(1, 0, 0), {glm::ivec3(1, 0, 0), glm::ivec3(1, 1, 0), glm::ivec3(1, 1, 1), glm::ivec3(1, 0, 1)}},
    {glm::ivec3(-1, 0, 0), {glm::ivec3(0, 0, 1), glm::ivec3(0, 1, 1), glm::ivec3(0, 1, 0), glm::ivec3(0, 0, 0)}},
    {glm::ivec3(0, 1, 0), {glm::ivec3(0, 1, 1), glm::ivec3(1, 1, 1), glm::ivec3(1, 1, 0), glm::ivec3(0, 1, 0)}},
    {glm::ivec3(0, -1, 0), {glm::ivec3(0, 0, 0), glm::ivec3(1, 0, 0), glm::ivec3(1, 0, 1), glm::ivec3(0, 0, 1)}},
    {glm::ivec3(0, 0, 1), {glm::ivec3(1, 0, 1), glm::ivec3(1, 1, 1), glm::ivec3(0, 1, 1), glm::ivec3(0, 0, 1)}},
    {glm::ivec3(0, 0, -1), {glm::ivec3(0, 0, 0), glm::ivec3(0, 1, 0), glm::ivec3(1, 1, 0), glm::ivec3(1, 0, 0)}},
}};

int bitIndex(int faceA, int faceB) {
    if (faceA > faceB) {
        std::swap(faceA, faceB);
    }
    return faceA * 6 + faceB;
}

ChunkFaceConnectivity computeFaceConnectivity(const Chunk& chunk) {
    constexpr int S = ChunkData::SIZE;

    std::array<uint8_t, S * S * S> air{};
    for (int x = 0; x < S; ++x)
        for (int y = 0; y < S; ++y)
            for (int z = 0; z < S; ++z)
                air[x * S * S + y * S + z] = (chunk.getBlock({x, y, z}).type == BlockType::Air) ? 1u : 0u;

    uint8_t reachable[6] = {};
    std::array<uint8_t, S * S * S> visited{};
    std::queue<int> q;

    for (int startFace = 0; startFace < 6; ++startFace) {
        std::fill(visited.begin(), visited.end(), 0);
        q = {};

        auto enqueue = [&](int x, int y, int z) {
            int idx = x * S * S + y * S + z;
            if (air[idx] && !visited[idx]) {
                visited[idx] = 1;
                q.push(idx);
            }
        };

        for (int a = 0; a < S; ++a) {
            for (int b = 0; b < S; ++b) {
                switch (startFace) {
                    case 0: enqueue(S - 1, a, b); break;
                    case 1: enqueue(0, a, b); break;
                    case 2: enqueue(a, S - 1, b); break;
                    case 3: enqueue(a, 0, b); break;
                    case 4: enqueue(a, b, S - 1); break;
                    case 5: enqueue(a, b, 0); break;
                }
            }
        }

        while (!q.empty()) {
            int idx = q.front();
            q.pop();
            int x = idx / (S * S);
            int y = (idx / S) % S;
            int z = idx % S;

            if (x == S - 1) reachable[startFace] |= (1 << 0);
            if (x == 0) reachable[startFace] |= (1 << 1);
            if (y == S - 1) reachable[startFace] |= (1 << 2);
            if (y == 0) reachable[startFace] |= (1 << 3);
            if (z == S - 1) reachable[startFace] |= (1 << 4);
            if (z == 0) reachable[startFace] |= (1 << 5);

            const int dx[] = {1, -1, 0, 0, 0, 0};
            const int dy[] = {0, 0, 1, -1, 0, 0};
            const int dz[] = {0, 0, 0, 0, 1, -1};
            for (int d = 0; d < 6; ++d) {
                int nx = x + dx[d], ny = y + dy[d], nz = z + dz[d];
                if (nx < 0 || nx >= S || ny < 0 || ny >= S || nz < 0 || nz >= S) continue;
                int nidx = nx * S * S + ny * S + nz;
                if (air[nidx] && !visited[nidx]) {
                    visited[nidx] = 1;
                    q.push(nidx);
                }
            }
        }
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

}  // namespace

bool chunkFacesConnected(ChunkFaceConnectivity mask, int faceA, int faceB) {
    if (faceA == faceB) {
        return false;
    }
    return (mask & (ChunkFaceConnectivity{1} << bitIndex(faceA, faceB))) != 0;
}

ChunkMesh buildChunkMesh(const VoxelWorld& voxelWorld, glm::ivec3 chunkPos) {
    MW_PROFILE_SCOPE("Client.BuildChunkMesh");

    const Chunk& chunk = voxelWorld.getChunk(chunkPos);
    const ShadedColorTable& colorTable = shadedColorTable();

    ChunkMesh mesh;
    mesh.vertices.reserve(1024);

    for (int x = 0; x < ChunkData::SIZE; ++x) {
        for (int y = 0; y < ChunkData::SIZE; ++y) {
            for (int z = 0; z < ChunkData::SIZE; ++z) {
                const glm::ivec3 localPos(x, y, z);
                const BlockData block = chunk.getBlock(localPos);
                if (block.type == BlockType::Air) {
                    continue;
                }
                assert(block.type < BlockType::Count);
                const std::array<uint32_t, 6>& faceColors = colorTable[static_cast<size_t>(block.type)];

                const glm::ivec3 worldPos = chunk.localToWorld(localPos);
                for (size_t faceIndex = 0; faceIndex < kFaces.size(); ++faceIndex) {
                    const Face& face = kFaces[faceIndex];
                    // Face culling
                    if (voxelWorld.getBlock(worldPos + face.normal).type != BlockType::Air) {
                        continue;
                    }

                    assert(mesh.vertices.size() + 4 <= kMaxChunkMeshVertices);

                    for (const glm::ivec3& corner : face.corners) {
                        const glm::ivec3 vertexPos = localPos + corner;
                        mesh.vertices.push_back(ChunkVertex{
                            static_cast<uint8_t>(vertexPos.x),
                            static_cast<uint8_t>(vertexPos.y),
                            static_cast<uint8_t>(vertexPos.z),
                            0,
                            faceColors[faceIndex],
                        });
                    }
                }
            }
        }
    }

    mesh.faceConnectivity = computeFaceConnectivity(chunk);
    MW_PROFILE_COUNTER("Client.ChunkMeshVertices", static_cast<int64_t>(mesh.vertices.size()));
    return mesh;
}
