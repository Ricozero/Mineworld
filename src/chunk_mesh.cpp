#include "chunk_mesh.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <queue>
#include <utility>

#include "block.h"
#include "chunk.h"
#include "helper.h"
#include "profiler.h"
#include "voxel_world.h"

namespace {

constexpr size_t kFloatsPerVertex = 4;

struct Face {
    glm::ivec3 normal;
    std::array<glm::vec3, 4> corners;
    float shade;
};

const std::array<Face, 6> kFaces = {{
    {glm::ivec3(1, 0, 0), {glm::vec3(1, 0, 0), glm::vec3(1, 1, 0), glm::vec3(1, 1, 1), glm::vec3(1, 0, 1)}, 0.82f},
    {glm::ivec3(-1, 0, 0), {glm::vec3(0, 0, 1), glm::vec3(0, 1, 1), glm::vec3(0, 1, 0), glm::vec3(0, 0, 0)}, 0.72f},
    {glm::ivec3(0, 1, 0), {glm::vec3(0, 1, 1), glm::vec3(1, 1, 1), glm::vec3(1, 1, 0), glm::vec3(0, 1, 0)}, 1.0f},
    {glm::ivec3(0, -1, 0), {glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), glm::vec3(1, 0, 1), glm::vec3(0, 0, 1)}, 0.55f},
    {glm::ivec3(0, 0, 1), {glm::vec3(1, 0, 1), glm::vec3(1, 1, 1), glm::vec3(0, 1, 1), glm::vec3(0, 0, 1)}, 0.9f},
    {glm::ivec3(0, 0, -1), {glm::vec3(0, 0, 0), glm::vec3(0, 1, 0), glm::vec3(1, 1, 0), glm::vec3(1, 0, 0)}, 0.65f},
}};

glm::vec3 blockColor(BlockType type) {
    switch (type) {
        case BlockType::Stone:
            return glm::vec3(0.48f, 0.50f, 0.53f);
        case BlockType::Dirt:
            return glm::vec3(0.26f, 0.17f, 0.10f);
        case BlockType::Grass:
            return glm::vec3(0.24f, 0.58f, 0.22f);
        case BlockType::Wood:
            return glm::vec3(0.50f, 0.31f, 0.14f);
        case BlockType::Leaves:
            return glm::vec3(0.16f, 0.45f, 0.18f);
        case BlockType::Water:
            return glm::vec3(0.20f, 0.42f, 0.85f);
        case BlockType::Sand:
            return glm::vec3(0.78f, 0.68f, 0.42f);
        case BlockType::Air:
            return glm::vec3(0.0f);
    }
    return glm::vec3(1.0f, 0.0f, 1.0f);
}

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
                air[x * S * S + y * S + z] =
                    (chunk.getBlock({x, y, z}).type == BlockType::Air) ? 1u : 0u;

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

void pushVertex(ChunkMesh& mesh, glm::vec3 position, uint32_t packedColor) {
    float colorAsFloat;
    std::memcpy(&colorAsFloat, &packedColor, sizeof(float));
    mesh.vertexData.push_back(position.x);
    mesh.vertexData.push_back(position.y);
    mesh.vertexData.push_back(position.z);
    mesh.vertexData.push_back(colorAsFloat);
    ++mesh.vertexCount;
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

    ChunkMesh mesh;
    mesh.vertexData.reserve(1024 * kFloatsPerVertex);
    mesh.indices.reserve(1536);

    for (int x = 0; x < ChunkData::SIZE; ++x) {
        for (int y = 0; y < ChunkData::SIZE; ++y) {
            for (int z = 0; z < ChunkData::SIZE; ++z) {
                const glm::ivec3 localPos(x, y, z);
                const BlockData block = chunk.getBlock(localPos);
                if (block.type == BlockType::Air) {
                    continue;
                }

                const glm::ivec3 worldPos = chunk.localToWorld(localPos);
                const glm::vec3 baseColor = blockColor(block.type);
                for (const Face& face : kFaces) {
                    // Face culling
                    if (voxelWorld.getBlock(worldPos + face.normal).type != BlockType::Air) {
                        continue;
                    }

                    if (mesh.vertexCount > kMaxMeshVertices - 4 || mesh.indices.size() > kMaxMeshIndices - 6) {
                        goto done;
                    }

                    const auto start = static_cast<uint16_t>(mesh.vertexCount);
                    const uint32_t packedColor = packColor(baseColor * face.shade);
                    for (const glm::vec3& corner : face.corners) {
                        pushVertex(mesh, glm::vec3(worldPos) + corner, packedColor);
                    }
                    mesh.indices.push_back(start + 0);
                    mesh.indices.push_back(start + 1);
                    mesh.indices.push_back(start + 2);
                    mesh.indices.push_back(start + 0);
                    mesh.indices.push_back(start + 2);
                    mesh.indices.push_back(start + 3);
                }
            }
        }
    }
done:

    mesh.faceConnectivity = computeFaceConnectivity(chunk);
    MW_PROFILE_COUNTER("Client.ChunkMeshVertices", static_cast<int64_t>(mesh.vertexCount));
    MW_PROFILE_COUNTER("Client.ChunkMeshIndices", static_cast<int64_t>(mesh.indices.size()));
    return mesh;
}
