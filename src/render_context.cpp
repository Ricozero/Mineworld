#include "render_context.h"

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/math.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <queue>
#include <unordered_set>
#include <vector>

#include "chunk.h"
#include "client_world.h"
#include "config.h"
#include "entity.h"
#include "log.h"
#include "profiler.h"

namespace {

template <typename T>
T cycleMode(T current) {
    return static_cast<T>((static_cast<int>(current) + 1) % static_cast<int>(T::Count));
}

constexpr bgfx::ViewId kMainView = 0;
constexpr bgfx::ViewId kImGuiView = 1;
constexpr uint32_t kResetFlags = BGFX_RESET_VSYNC;
constexpr size_t kBoxVertexCount = 24;
constexpr size_t kBoxIndexCount = 36;
constexpr size_t kPlayerModelVertexCount = kBoxVertexCount * 2;
constexpr size_t kPlayerModelIndexCount = kBoxIndexCount * 2;
constexpr size_t kMaxBatchVertices = UINT16_MAX;
constexpr size_t kMaxBatchIndices = UINT16_MAX;

struct PosColorVertex {
    float x;
    float y;
    float z;
    uint32_t abgr;

    static bgfx::VertexLayout layout;
};

bgfx::VertexLayout PosColorVertex::layout;

struct ImGuiVertex {
    static bgfx::VertexLayout layout;
};

bgfx::VertexLayout ImGuiVertex::layout;

struct MeshBuilder {
    std::vector<PosColorVertex> vertices;
    std::vector<uint16_t> indices;
};

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

uint32_t packColor(glm::vec3 color) {
    color = glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f));
    const uint32_t r = static_cast<uint32_t>(color.r * 255.0f);
    const uint32_t g = static_cast<uint32_t>(color.g * 255.0f);
    const uint32_t b = static_cast<uint32_t>(color.b * 255.0f);
    return 0xff000000u | (b << 16) | (g << 8) | r;
}

constexpr int kOppositeFace[6] = {1, 0, 3, 2, 5, 4};

constexpr glm::ivec3 kFaceDir[6] = {
    {1, 0, 0},
    {-1, 0, 0},
    {0, 1, 0},
    {0, -1, 0},
    {0, 0, 1},
    {0, 0, -1},
};

inline int bitIndex(int from, int to) { return from * 6 + to; }
inline bool faceConnected(ChunkFaceConnectivity mask, int from, int to) {
    return (mask >> bitIndex(from, to)) & 1u;
}

ChunkFaceConnectivity computeFaceConnectivity(const Chunk& chunk) {
    constexpr int S = Chunk::SIZE;

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
    for (int f = 0; f < 6; ++f)
        for (int t = 0; t < 6; ++t)
            if ((reachable[f] >> t) & 1)
                mask |= (1u << bitIndex(f, t)) | (1u << bitIndex(t, f));
    return mask;
}

struct Frustum {
    glm::vec4 planes[6];  // left, right, bottom, top, near, far

    // Gribb/Hartmann extraction from a column-major view-projection float[16].
    static Frustum fromBxMatrix(const float* vp) {
        glm::mat4 m;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                m[c][r] = vp[c * 4 + r];

        const glm::mat4 t = glm::transpose(m);
        Frustum f;
        f.planes[0] = t[3] + t[0];
        f.planes[1] = t[3] - t[0];
        f.planes[2] = t[3] + t[1];
        f.planes[3] = t[3] - t[1];
        f.planes[4] = t[3] + t[2];
        f.planes[5] = t[3] - t[2];
        for (auto& p : f.planes) {
            float len = glm::length(glm::vec3(p));
            if (len > 0.0f) p /= len;
        }
        return f;
    }

    // Returns true if AABB [min,max] intersects or is inside the frustum.
    bool testAABB(glm::vec3 mn, glm::vec3 mx) const {
        for (const auto& p : planes) {
            // Positive vertex (furthest in plane normal direction)
            glm::vec3 pv(p.x > 0 ? mx.x : mn.x,
                         p.y > 0 ? mx.y : mn.y,
                         p.z > 0 ? mx.z : mn.z);
            if (glm::dot(glm::vec3(p), pv) + p.w < 0.0f) return false;
        }
        return true;
    }
};

void addQuad(MeshBuilder& mesh, const std::array<glm::vec3, 4>& corners, glm::vec3 color) {
    if (mesh.vertices.size() > kMaxBatchVertices - 4) {
        return;
    }

    const auto start = static_cast<uint16_t>(mesh.vertices.size());
    const uint32_t packedColor = packColor(color);
    for (const glm::vec3& corner : corners) {
        mesh.vertices.push_back(PosColorVertex{corner.x, corner.y, corner.z, packedColor});
    }

    mesh.indices.push_back(start + 0);
    mesh.indices.push_back(start + 1);
    mesh.indices.push_back(start + 2);
    mesh.indices.push_back(start + 0);
    mesh.indices.push_back(start + 2);
    mesh.indices.push_back(start + 3);
}

glm::vec3 rotateYaw(glm::vec3 point, float yawDegrees) {
    const float yaw = glm::radians(yawDegrees);
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    return glm::vec3(point.x * c - point.z * s, point.y, point.x * s + point.z * c);
}

glm::vec3 rotatePitchThenYaw(glm::vec3 point, float yawDegrees, float pitchDegrees) {
    const float pitch = glm::radians(pitchDegrees);
    const float c = std::cos(pitch);
    const float s = std::sin(pitch);
    const glm::vec3 pitched(point.x * c - point.y * s, point.x * s + point.y * c, point.z);
    return rotateYaw(pitched, yawDegrees);
}

void addOrientedBox(MeshBuilder& mesh, glm::vec3 center, glm::vec3 halfSize, float yawDegrees, glm::vec3 color) {
    const std::array<glm::vec3, 8> local = {{
        {-halfSize.x, -halfSize.y, -halfSize.z},
        {halfSize.x, -halfSize.y, -halfSize.z},
        {halfSize.x, halfSize.y, -halfSize.z},
        {-halfSize.x, halfSize.y, -halfSize.z},
        {-halfSize.x, -halfSize.y, halfSize.z},
        {halfSize.x, -halfSize.y, halfSize.z},
        {halfSize.x, halfSize.y, halfSize.z},
        {-halfSize.x, halfSize.y, halfSize.z},
    }};

    std::array<glm::vec3, 8> v{};
    for (size_t i = 0; i < local.size(); ++i) {
        v[i] = center + rotateYaw(local[i], yawDegrees);
    }

    addQuad(mesh, {{v[0], v[3], v[2], v[1]}}, color * 0.65f);
    addQuad(mesh, {{v[4], v[5], v[6], v[7]}}, color * 0.90f);
    addQuad(mesh, {{v[0], v[1], v[5], v[4]}}, color * 0.55f);
    addQuad(mesh, {{v[3], v[7], v[6], v[2]}}, color);
    addQuad(mesh, {{v[1], v[2], v[6], v[5]}}, color * 0.82f);
    addQuad(mesh, {{v[0], v[4], v[7], v[3]}}, color * 0.72f);
}

void addHeadBox(MeshBuilder& mesh, glm::vec3 neckPosition, glm::vec3 localCenter, glm::vec3 halfSize, float yawDegrees, float pitchDegrees, glm::vec3 color) {
    const std::array<glm::vec3, 8> local = {{
        localCenter + glm::vec3(-halfSize.x, -halfSize.y, -halfSize.z),
        localCenter + glm::vec3(halfSize.x, -halfSize.y, -halfSize.z),
        localCenter + glm::vec3(halfSize.x, halfSize.y, -halfSize.z),
        localCenter + glm::vec3(-halfSize.x, halfSize.y, -halfSize.z),
        localCenter + glm::vec3(-halfSize.x, -halfSize.y, halfSize.z),
        localCenter + glm::vec3(halfSize.x, -halfSize.y, halfSize.z),
        localCenter + glm::vec3(halfSize.x, halfSize.y, halfSize.z),
        localCenter + glm::vec3(-halfSize.x, halfSize.y, halfSize.z),
    }};

    std::array<glm::vec3, 8> v{};
    for (size_t i = 0; i < local.size(); ++i) {
        v[i] = neckPosition + rotatePitchThenYaw(local[i], yawDegrees, pitchDegrees);
    }

    const glm::vec3 faceColor(0.95f, 0.12f, 0.10f);
    addQuad(mesh, {{v[0], v[3], v[2], v[1]}}, color * 0.65f);
    addQuad(mesh, {{v[4], v[5], v[6], v[7]}}, color * 0.90f);
    addQuad(mesh, {{v[0], v[1], v[5], v[4]}}, color * 0.55f);
    addQuad(mesh, {{v[3], v[7], v[6], v[2]}}, color);
    addQuad(mesh, {{v[1], v[2], v[6], v[5]}}, faceColor);
    addQuad(mesh, {{v[0], v[4], v[7], v[3]}}, color * 0.72f);
}

void addPlayerModel(MeshBuilder& mesh, const TransformComponent& transform, glm::vec3 color) {
    const float yaw = transform.rotation.y;
    const float pitch = std::clamp(transform.rotation.x, -60.0f, 60.0f);

    const glm::vec3 bodyCenter = transform.position + glm::vec3(0.0f, 0.65f, 0.0f);
    addOrientedBox(mesh, bodyCenter, glm::vec3(0.30f, 0.62f, 0.22f), yaw, color);

    const glm::vec3 neckPosition = transform.position + glm::vec3(0.0f, 1.25f, 0.0f);
    addHeadBox(mesh, neckPosition, glm::vec3(0.0f, 0.30f, 0.0f), glm::vec3(0.28f), yaw, pitch, color * 1.12f);
}

void addLineBox(MeshBuilder& mesh, glm::vec3 min, glm::vec3 max, glm::vec3 color) {
    if (mesh.vertices.size() > kMaxBatchVertices - 8) {
        return;
    }

    const uint16_t start = static_cast<uint16_t>(mesh.vertices.size());
    const uint32_t packedColor = packColor(color);
    const std::array<glm::vec3, 8> v = {{
        {min.x, min.y, min.z},
        {max.x, min.y, min.z},
        {max.x, max.y, min.z},
        {min.x, max.y, min.z},
        {min.x, min.y, max.z},
        {max.x, min.y, max.z},
        {max.x, max.y, max.z},
        {min.x, max.y, max.z},
    }};
    for (const glm::vec3& corner : v) {
        mesh.vertices.push_back(PosColorVertex{corner.x, corner.y, corner.z, packedColor});
    }

    const std::array<std::pair<uint16_t, uint16_t>, 12> edges = {{
        {0, 1},
        {1, 2},
        {2, 3},
        {3, 0},
        {4, 5},
        {5, 6},
        {6, 7},
        {7, 4},
        {0, 4},
        {1, 5},
        {2, 6},
        {3, 7},
    }};
    for (const auto& edge : edges) {
        mesh.indices.push_back(start + edge.first);
        mesh.indices.push_back(start + edge.second);
    }
}

void submitLineBatch(const MeshBuilder& mesh, unsigned short programIndex) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return;
    }
    const uint32_t vertexCount = static_cast<uint32_t>(mesh.vertices.size());
    const uint32_t indexCount = static_cast<uint32_t>(mesh.indices.size());
    if (vertexCount > kMaxBatchVertices || indexCount > kMaxBatchIndices) {
        return;
    }
    if (bgfx::getAvailTransientVertexBuffer(vertexCount, PosColorVertex::layout) >= vertexCount &&
        bgfx::getAvailTransientIndexBuffer(indexCount) >= indexCount) {
        bgfx::TransientVertexBuffer vertexBuffer;
        bgfx::TransientIndexBuffer indexBuffer;
        bgfx::allocTransientVertexBuffer(&vertexBuffer, vertexCount, PosColorVertex::layout);
        bgfx::allocTransientIndexBuffer(&indexBuffer, indexCount);
        std::memcpy(vertexBuffer.data, mesh.vertices.data(), mesh.vertices.size() * sizeof(PosColorVertex));
        std::memcpy(indexBuffer.data, mesh.indices.data(), mesh.indices.size() * sizeof(uint16_t));

        float model[16];
        bx::mtxIdentity(model);
        bgfx::setTransform(model);
        bgfx::setVertexBuffer(0, &vertexBuffer);
        bgfx::setIndexBuffer(&indexBuffer);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_PT_LINES);
        bgfx::submit(kMainView, bgfx::ProgramHandle{programIndex});
        MW_PROFILE_COUNTER("Client.Render.LineSubmits", 1);
        MW_PROFILE_COUNTER("Client.Render.LineVertices", static_cast<int64_t>(vertexCount));
        MW_PROFILE_COUNTER("Client.Render.LineIndices", static_cast<int64_t>(indexCount));
    }
}

std::vector<uint8_t> readBinaryFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }

    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return {};
    }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

const char* shaderDirectoryForRenderer(bgfx::RendererType::Enum renderer) {
    switch (renderer) {
        case bgfx::RendererType::Direct3D11:
        case bgfx::RendererType::Direct3D12:
            return "dx11";
        case bgfx::RendererType::OpenGL:
            return "glsl";
        case bgfx::RendererType::OpenGLES:
            return "essl";
        case bgfx::RendererType::Vulkan:
            return "spirv";
        default:
            return "dx11";
    }
}

double timestampRangeMs(int64_t begin, int64_t end, int64_t frequency) {
    if (frequency <= 0 || end <= begin) {
        return 0.0;
    }
    return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(frequency);
}

double timestampMs(int64_t value, int64_t frequency) {
    if (frequency <= 0 || value <= 0) {
        return 0.0;
    }
    return static_cast<double>(value) * 1000.0 / static_cast<double>(frequency);
}

void recordBgfxStats(const bgfx::Stats* stats) {
    if (!stats) {
        return;
    }

    MW_PROFILE_GAUGE("BGFX.CPUFrameMs", timestampMs(stats->cpuTimeFrame, stats->cpuTimerFreq));
    MW_PROFILE_GAUGE("BGFX.CPUSubmitMs", timestampRangeMs(stats->cpuTimeBegin, stats->cpuTimeEnd, stats->cpuTimerFreq));
    MW_PROFILE_GAUGE("BGFX.GPUFrameMs", timestampRangeMs(stats->gpuTimeBegin, stats->gpuTimeEnd, stats->gpuTimerFreq));
    MW_PROFILE_GAUGE("BGFX.WaitRenderMs", timestampMs(stats->waitRender, stats->cpuTimerFreq));
    MW_PROFILE_GAUGE("BGFX.WaitSubmitMs", timestampMs(stats->waitSubmit, stats->cpuTimerFreq));
    MW_PROFILE_GAUGE("BGFX.DrawCalls", static_cast<double>(stats->numDraw));
    MW_PROFILE_GAUGE("BGFX.TransientVB", static_cast<double>(stats->transientVbUsed));
    MW_PROFILE_GAUGE("BGFX.TransientIB", static_cast<double>(stats->transientIbUsed));
    MW_PROFILE_GAUGE("BGFX.ComputeCalls", static_cast<double>(stats->numCompute));
    MW_PROFILE_GAUGE("BGFX.BlitCalls", static_cast<double>(stats->numBlit));
    MW_PROFILE_GAUGE("BGFX.GpuLatency", static_cast<double>(stats->maxGpuLatency));
    MW_PROFILE_GAUGE("BGFX.GpuMemUsedMB", static_cast<double>(stats->gpuMemoryUsed) / (1024.0 * 1024.0));
    MW_PROFILE_GAUGE("BGFX.GpuMemMaxMB", static_cast<double>(stats->gpuMemoryMax) / (1024.0 * 1024.0));
}

void submitMeshBatch(const MeshBuilder& mesh, unsigned short programIndex) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return;
    }
    const uint32_t vertexCount = static_cast<uint32_t>(mesh.vertices.size());
    const uint32_t indexCount = static_cast<uint32_t>(mesh.indices.size());
    if (vertexCount > kMaxBatchVertices || indexCount > kMaxBatchIndices) {
        return;
    }
    if (bgfx::getAvailTransientVertexBuffer(vertexCount, PosColorVertex::layout) >= vertexCount &&
        bgfx::getAvailTransientIndexBuffer(indexCount) >= indexCount) {
        bgfx::TransientVertexBuffer vertexBuffer;
        bgfx::TransientIndexBuffer indexBuffer;
        bgfx::allocTransientVertexBuffer(&vertexBuffer, vertexCount, PosColorVertex::layout);
        bgfx::allocTransientIndexBuffer(&indexBuffer, indexCount);
        std::memcpy(vertexBuffer.data, mesh.vertices.data(), mesh.vertices.size() * sizeof(PosColorVertex));
        std::memcpy(indexBuffer.data, mesh.indices.data(), mesh.indices.size() * sizeof(uint16_t));

        float model[16];
        bx::mtxIdentity(model);
        bgfx::setTransform(model);
        bgfx::setVertexBuffer(0, &vertexBuffer);
        bgfx::setIndexBuffer(&indexBuffer);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
        bgfx::submit(kMainView, bgfx::ProgramHandle{programIndex});
        MW_PROFILE_COUNTER("Client.Render.MeshSubmits", 1);
        MW_PROFILE_COUNTER("Client.Render.MeshVertices", static_cast<int64_t>(vertexCount));
        MW_PROFILE_COUNTER("Client.Render.MeshIndices", static_cast<int64_t>(indexCount));
    }
}

}  // namespace

bool ChunkMeshCache::contains(glm::ivec3 chunkPos) const {
    return entries_.count(chunkPos) > 0;
}

const ChunkMeshCache::Entry* ChunkMeshCache::get(glm::ivec3 chunkPos) const {
    auto it = entries_.find(chunkPos);
    return it != entries_.end() ? &it->second : nullptr;
}

void ChunkMeshCache::put(glm::ivec3 chunkPos, size_t blockCount, Entry entry) {
    blockCounts_[chunkPos] = blockCount;
    entries_[chunkPos] = std::move(entry);
}

void ChunkMeshCache::invalidate(glm::ivec3 chunkPos) {
    entries_.erase(chunkPos);
    blockCounts_.erase(chunkPos);
}

void ChunkMeshCache::evictStale(const std::vector<glm::ivec3>& loadedChunks) {
    std::unordered_set<glm::ivec3> loadedSet(loadedChunks.begin(), loadedChunks.end());
    std::vector<glm::ivec3> toRemove;
    toRemove.reserve(entries_.size());
    for (const auto& [pos, _] : entries_) {
        if (!loadedSet.count(pos)) {
            toRemove.push_back(pos);
        }
    }
    for (const glm::ivec3& pos : toRemove) {
        invalidate(pos);
    }
}

bool ChunkMeshCache::needsRebuild(glm::ivec3 chunkPos, size_t currentBlockCount,
                                  const std::unordered_map<glm::ivec3, size_t>& currentCounts) const {
    auto countIt = blockCounts_.find(chunkPos);
    if (countIt == blockCounts_.end() || countIt->second != currentBlockCount || !contains(chunkPos)) {
        return true;
    }

    static const std::array<glm::ivec3, 6> kNeighborOffsets = {{
        glm::ivec3(1, 0, 0),
        glm::ivec3(-1, 0, 0),
        glm::ivec3(0, 1, 0),
        glm::ivec3(0, -1, 0),
        glm::ivec3(0, 0, 1),
        glm::ivec3(0, 0, -1),
    }};
    for (const glm::ivec3& off : kNeighborOffsets) {
        const glm::ivec3 neighbor = chunkPos + off;
        auto prevIt = blockCounts_.find(neighbor);
        auto currIt = currentCounts.find(neighbor);
        if (prevIt != blockCounts_.end() && currIt != currentCounts.end()) {
            if (prevIt->second != currIt->second) {
                return true;
            }
        }
    }
    return false;
}

RenderContext::~RenderContext() {
    shutdown();
}

bool RenderContext::initialize(int width, int height, const char* title) {
    framebufferWidth_ = width;
    framebufferHeight_ = height;
    windowWidth_ = width;
    windowHeight_ = height;

    if (!glfwInit()) {
        logging::error("Failed to initialize GLFW");
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window_ = glfwCreateWindow(windowWidth_, windowHeight_, title, nullptr, nullptr);
    if (!window_) {
        logging::error("Failed to create GLFW window");
        glfwTerminate();
        return false;
    }

    bgfx::Init init;
    const std::string& api = AppConfig::instance().graphicsApi;
    if (api == "dx11") init.type = bgfx::RendererType::Direct3D11;
    else if (api == "dx12") init.type = bgfx::RendererType::Direct3D12;
    else if (api == "opengl") init.type = bgfx::RendererType::OpenGL;
    else if (api == "vulkan") init.type = bgfx::RendererType::Vulkan;
    else init.type = bgfx::RendererType::Direct3D11;
    init.platformData.nwh = glfwGetWin32Window(window_);
    init.resolution.width = static_cast<uint32_t>(framebufferWidth_);
    init.resolution.height = static_cast<uint32_t>(framebufferHeight_);
    init.resolution.reset = kResetFlags;
    if (!bgfx::init(init)) {
        logging::error("Failed to initialize bgfx");
        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
        return false;
    }
    bgfxInitialized_ = true;

    PosColorVertex::layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();

    ImGuiVertex::layout.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();

    if (!loadShaders()) {
        shutdown();
        return false;
    }

    if (!initializeImGui()) {
        shutdown();
        return false;
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetScrollCallback(window_, RenderContext::handleScroll);
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    logging::info("Renderer initialized with GLFW/bgfx ({})", bgfx::getRendererName(bgfx::getRendererType()));
    return true;
}

void RenderContext::shutdown() {
    shutdownImGui();
    destroyShaders();

    if (bgfxInitialized_) {
        bgfx::shutdown();
        bgfxInitialized_ = false;
    }

    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
    }
}

bool RenderContext::shouldClose() const {
    return !window_ || glfwWindowShouldClose(window_);
}

RenderContext::StartMenuAction RenderContext::renderStartMenu(char* addressBuffer, size_t addressBufferSize, int& port) {
    if (!window_ || !bgfxInitialized_) {
        return StartMenuAction::None;
    }

    releaseMouse();

    int windowWidth = 0;
    int windowHeight = 0;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetWindowSize(window_, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(window_, &framebufferWidth, &framebufferHeight);
    windowWidth_ = std::max(windowWidth, 1);
    windowHeight_ = std::max(windowHeight, 1);
    framebufferWidth_ = std::max(framebufferWidth, 1);
    framebufferHeight_ = std::max(framebufferHeight, 1);
    framebufferScaleX_ = static_cast<float>(framebufferWidth_) / static_cast<float>(windowWidth_);
    framebufferScaleY_ = static_cast<float>(framebufferHeight_) / static_cast<float>(windowHeight_);
    bgfx::setViewRect(kMainView, 0, 0, static_cast<uint16_t>(framebufferWidth_), static_cast<uint16_t>(framebufferHeight_));
    bgfx::setViewClear(kMainView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x1b2533ff, 1.0f, 0);
    bgfx::touch(kMainView);

    StartMenuAction action = StartMenuAction::None;
    if (imguiContext_) {
        ImGui::SetCurrentContext(imguiContext_);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(windowWidth_), static_cast<float>(windowHeight_));
        io.DisplayFramebufferScale = ImVec2(framebufferScaleX_, framebufferScaleY_);
        io.DeltaTime = 1.0f / 60.0f;
        updateImGuiInput();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(windowWidth_ * 0.5f, windowHeight_ * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_Always);
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin("Mineworld", nullptr, flags)) {
            ImGui::TextUnformatted("Local");
            if (ImGui::Button("Start", ImVec2(-1.0f, 36.0f))) {
                action = StartMenuAction::Local;
            }
            ImGui::Spacing();
            ImGui::TextUnformatted("Remote Mode");
            ImGui::InputText("IP", addressBuffer, addressBufferSize);
            ImGui::InputInt("Port", &port);
            port = std::clamp(port, 1, 65535);
            if (ImGui::Button("Connect", ImVec2(-1.0f, 36.0f))) {
                action = StartMenuAction::Remote;
            }
            ImGui::Spacing();
            if (ImGui::Button("Quit Game", ImVec2(-1.0f, 36.0f))) {
                action = StartMenuAction::Quit;
            }
        }
        ImGui::End();
        ImGui::Render();
        renderImGuiDrawData(ImGui::GetDrawData());
    }

    bgfx::frame();
    return action;
}

RenderContext::ConnectingAction RenderContext::renderConnecting(const std::string& address, uint16_t port) {
    if (!window_ || !bgfxInitialized_) {
        return ConnectingAction::None;
    }

    releaseMouse();

    int windowWidth = 0;
    int windowHeight = 0;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetWindowSize(window_, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(window_, &framebufferWidth, &framebufferHeight);
    windowWidth_ = std::max(windowWidth, 1);
    windowHeight_ = std::max(windowHeight, 1);
    framebufferWidth_ = std::max(framebufferWidth, 1);
    framebufferHeight_ = std::max(framebufferHeight, 1);
    framebufferScaleX_ = static_cast<float>(framebufferWidth_) / static_cast<float>(windowWidth_);
    framebufferScaleY_ = static_cast<float>(framebufferHeight_) / static_cast<float>(windowHeight_);
    bgfx::setViewRect(kMainView, 0, 0, static_cast<uint16_t>(framebufferWidth_), static_cast<uint16_t>(framebufferHeight_));
    bgfx::setViewClear(kMainView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x1b2533ff, 1.0f, 0);
    bgfx::touch(kMainView);

    ConnectingAction action = ConnectingAction::None;
    if (imguiContext_) {
        ImGui::SetCurrentContext(imguiContext_);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(windowWidth_), static_cast<float>(windowHeight_));
        io.DisplayFramebufferScale = ImVec2(framebufferScaleX_, framebufferScaleY_);
        io.DeltaTime = 1.0f / 60.0f;
        updateImGuiInput();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(windowWidth_ * 0.5f, windowHeight_ * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_Always);
        if (ImGui::Begin("Connecting", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::Text("Connecting to %s:%u", address.c_str(), static_cast<unsigned>(port));
            ImGui::TextUnformatted("Waiting for server hello...");
            ImGui::Spacing();
            if (ImGui::Button("Cancel", ImVec2(-1.0f, 36.0f))) {
                action = ConnectingAction::Cancel;
            }
        }
        ImGui::End();
        ImGui::Render();
        renderImGuiDrawData(ImGui::GetDrawData());
    }

    bgfx::frame();
    return action;
}

void RenderContext::pollEvents() {
    MW_PROFILE_SCOPE("Client.PollEvents");

    glfwPollEvents();
}

void RenderContext::processInput(float deltaTime, glm::vec3& rotation, PlayerComponent& player, ControllerInputComponent& input) {
    MW_PROFILE_SCOPE("Client.ProcessInput");

    if (!window_) {
        return;
    }

    deltaTime = std::clamp(deltaTime, 0.0f, 0.05f);

    const bool escapeDown = glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS;
    if (escapeDown && !prevEscapeDown_) {
        inGameMenuOpen_ = !inGameMenuOpen_;
        if (inGameMenuOpen_) {
            releaseMouse();
        } else {
            captureMouse();
        }
    }
    prevEscapeDown_ = escapeDown;

    if (inGameMenuOpen_) {
        input.move = glm::vec3(0.0f);
        input.jump = false;
        input.sprint = false;
        return;
    }

    // Hold Alt to release mouse, release Alt to recapture
    const bool altHeld = glfwGetKey(window_, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
                         glfwGetKey(window_, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;

    if (altHeld && mouseCaptured_) {
        // Release mouse
        mouseCaptured_ = false;
        glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    } else if (!altHeld && !mouseCaptured_) {
        // Recapture mouse
        mouseCaptured_ = true;
        glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        hasMousePosition_ = false;  // prevent camera jump on recapture
    }

    // While mouse is released, only handle Escape and function keys
    if (!mouseCaptured_) {
        const bool f1Down = glfwGetKey(window_, GLFW_KEY_F1) == GLFW_PRESS;
        if (f1Down && !prevF1Down_) {
            profilerMode_ = cycleMode(profilerMode_);
        }
        prevF1Down_ = f1Down;

        const bool f2Down = glfwGetKey(window_, GLFW_KEY_F2) == GLFW_PRESS;
        if (f2Down && !prevF2Down_) {
            cursorMode_ = cycleMode(cursorMode_);
        }
        prevF2Down_ = f2Down;

        const bool f3Down = glfwGetKey(window_, GLFW_KEY_F3) == GLFW_PRESS;
        if (f3Down && !prevF3Down_) {
            showChunkBounds_ = !showChunkBounds_;
        }
        prevF3Down_ = f3Down;

        const bool f5Down = glfwGetKey(window_, GLFW_KEY_F5) == GLFW_PRESS;
        if (f5Down && !prevF5Down_ && player.mode == PlayerMode::Survival) {
            cameraViewMode_ = cycleMode(cameraViewMode_);
        }
        prevF5Down_ = f5Down;

        return;
    }

    double mouseX = 0.0;
    double mouseY = 0.0;
    glfwGetCursorPos(window_, &mouseX, &mouseY);
    if (!hasMousePosition_) {
        lastMouseX_ = mouseX;
        lastMouseY_ = mouseY;
        hasMousePosition_ = true;
    }

    constexpr float mouseSensitivity = 0.12f;
    const float mouseDeltaX = static_cast<float>(mouseX - lastMouseX_);
    const float mouseDeltaY = static_cast<float>(mouseY - lastMouseY_);
    lastMouseX_ = mouseX;
    lastMouseY_ = mouseY;

    // Update rotation: rotation.x = pitch, rotation.y = yaw
    rotation.y += mouseDeltaX * mouseSensitivity;
    rotation.x -= mouseDeltaY * mouseSensitivity;
    rotation.x = std::clamp(rotation.x, -88.0f, 88.0f);

    // Also update internal camera state (used by forward()/right() helpers and render)
    cameraYaw_ = rotation.y;
    cameraPitch_ = rotation.x;

    const bool f4Down = glfwGetKey(window_, GLFW_KEY_F4) == GLFW_PRESS;
    if (f4Down && !prevF4Down_) {
        player.mode = player.mode == PlayerMode::Spectator ? PlayerMode::Survival : PlayerMode::Spectator;
        logging::info("Switched player mode to {}", player.mode == PlayerMode::Spectator ? "spectator" : "survival");
    }
    prevF4Down_ = f4Down;

    const bool f5Down = glfwGetKey(window_, GLFW_KEY_F5) == GLFW_PRESS;
    if (f5Down && !prevF5Down_ && player.mode == PlayerMode::Survival) {
        cameraViewMode_ = cycleMode(cameraViewMode_);
        logging::info("Switched camera view to {}",
                      cameraViewMode_ == CameraViewMode::FirstPerson        ? "first-person"
                      : cameraViewMode_ == CameraViewMode::ThirdPersonFront ? "third-person-front"
                                                                            : "third-person-back");
    }
    prevF5Down_ = f5Down;

    const bool spectatorMode = player.mode == PlayerMode::Spectator;
    input.move = glm::vec3(0.0f);
    input.jump = false;
    input.sprint = glfwGetKey(window_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;

    if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) {
        input.move.z += 1.0f;
    }
    if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) {
        input.move.z -= 1.0f;
    }
    if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) {
        input.move.x -= 1.0f;
    }
    if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) {
        input.move.x += 1.0f;
    }
    if (glm::dot(input.move, input.move) > 1.0f) {
        input.move = glm::normalize(input.move);
    }
    const bool spaceDown = glfwGetKey(window_, GLFW_KEY_SPACE) == GLFW_PRESS;
    if (!spectatorMode && spaceDown && !prevSpaceDown_) {
        input.jump = true;
    }
    if (spectatorMode && glfwGetKey(window_, GLFW_KEY_SPACE) == GLFW_PRESS) {
        input.move.y += 1.0f;
    }
    prevSpaceDown_ = spaceDown;
    if (spectatorMode && glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
        input.move.y -= 1.0f;
    }
    const bool f1Down = glfwGetKey(window_, GLFW_KEY_F1) == GLFW_PRESS;
    if (f1Down && !prevF1Down_) {
        profilerMode_ = cycleMode(profilerMode_);
    }
    prevF1Down_ = f1Down;

    const bool f2Down = glfwGetKey(window_, GLFW_KEY_F2) == GLFW_PRESS;
    if (f2Down && !prevF2Down_) {
        cursorMode_ = cycleMode(cursorMode_);
    }
    prevF2Down_ = f2Down;

    const bool f3Down = glfwGetKey(window_, GLFW_KEY_F3) == GLFW_PRESS;
    if (f3Down && !prevF3Down_) {
        showChunkBounds_ = !showChunkBounds_;
    }
    prevF3Down_ = f3Down;
}

void RenderContext::setCamera(const glm::vec3& position, float yaw, float pitch, PlayerMode mode, uint32_t localSessionId) {
    localSessionId_ = localSessionId;
    cameraYaw_ = yaw;
    cameraPitch_ = pitch;

    if (mode == PlayerMode::Spectator) {
        cameraViewMode_ = CameraViewMode::FirstPerson;
        cameraPosition_ = position;
        return;
    }

    constexpr float eyeHeight = 1.62f;
    constexpr float cameraDistance = 4.0f;
    constexpr float thirdPersonTargetHeight = 0.85f;
    const glm::vec3 eyePosition = position + glm::vec3(0.0f, eyeHeight, 0.0f);
    const glm::vec3 thirdPersonTarget = position + glm::vec3(0.0f, thirdPersonTargetHeight, 0.0f);

    switch (cameraViewMode_) {
        case CameraViewMode::FirstPerson:
            cameraPosition_ = eyePosition;
            break;
        case CameraViewMode::ThirdPersonFront: {
            cameraYaw_ = yaw + 180.0f;
            cameraPitch_ = -pitch;
            cameraPosition_ = thirdPersonTarget - forward() * cameraDistance;
            break;
        }
        case CameraViewMode::ThirdPersonBack:
            cameraPitch_ = pitch;
            cameraPosition_ = thirdPersonTarget - forward() * cameraDistance;
            break;
        default:
            break;
    }
}

void RenderContext::render(const ClientWorld& world) {
    MW_PROFILE_SCOPE("Client.Render");

    if (!window_ || !bgfxInitialized_) {
        return;
    }

    // Compute deltaTime internally
    const auto now = std::chrono::steady_clock::now();
    float deltaTime = 1.0f / 60.0f;
    if (hasLastRenderTime_) {
        const std::chrono::duration<float> elapsed = now - lastRenderTime_;
        deltaTime = std::clamp(elapsed.count(), 1.0f / 1000.0f, 0.1f);
    }
    lastRenderTime_ = now;
    hasLastRenderTime_ = true;

    int windowWidth = 0;
    int windowHeight = 0;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetWindowSize(window_, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(window_, &framebufferWidth, &framebufferHeight);

    windowWidth = std::max(windowWidth, 1);
    windowHeight = std::max(windowHeight, 1);
    framebufferWidth = std::max(framebufferWidth, 1);
    framebufferHeight = std::max(framebufferHeight, 1);

    if (framebufferWidth != framebufferWidth_ || framebufferHeight != framebufferHeight_) {
        bgfx::reset(static_cast<uint32_t>(framebufferWidth), static_cast<uint32_t>(framebufferHeight), kResetFlags);
    }

    framebufferWidth_ = framebufferWidth;
    framebufferHeight_ = framebufferHeight;
    windowWidth_ = windowWidth;
    windowHeight_ = windowHeight;
    framebufferScaleX_ = static_cast<float>(framebufferWidth) / static_cast<float>(windowWidth);
    framebufferScaleY_ = static_cast<float>(framebufferHeight) / static_cast<float>(windowHeight);

    bgfx::setViewRect(kMainView, 0, 0, static_cast<uint16_t>(framebufferWidth_), static_cast<uint16_t>(framebufferHeight_));
    bgfx::setViewClear(kMainView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x87bdf2ff, 1.0f, 0);

    const glm::vec3 target = cameraPosition_ + forward();
    const bx::Vec3 eye(cameraPosition_.x, cameraPosition_.y, cameraPosition_.z);
    const bx::Vec3 at(target.x, target.y, target.z);
    float view[16];
    float projection[16];
    bx::mtxLookAt(view, eye, at, bx::Vec3(0.0f, 1.0f, 0.0f), bx::Handedness::Right);
    bx::mtxProj(
        projection,
        70.0f,
        static_cast<float>(framebufferWidth_) / framebufferHeight_,
        0.1f,
        500.0f,
        bgfx::getCaps()->homogeneousDepth,
        bx::Handedness::Right);
    bgfx::setViewTransform(kMainView, view, projection);
    bgfx::touch(kMainView);

    renderWorld(world);

    recordBgfxStats(bgfx::getStats());

    const bool anyOverlay = profilerMode_ != ProfilerMode::Hidden || cursorMode_ != CursorMode::Hidden || inGameMenuOpen_;
    if (anyOverlay && imguiContext_) {
        ImGui::SetCurrentContext(imguiContext_);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(windowWidth_), static_cast<float>(windowHeight_));
        io.DisplayFramebufferScale = ImVec2(framebufferScaleX_, framebufferScaleY_);
        io.DeltaTime = deltaTime > 0.0f ? deltaTime : 1.0f / 60.0f;
        updateImGuiInput();
        ImGui::NewFrame();

        if (profilerMode_ != ProfilerMode::Hidden) {
            renderProfilerOverlay();
        }
        if (cursorMode_ != CursorMode::Hidden) {
            renderCursorOverlay();
        }
        if (inGameMenuOpen_) {
            renderInGameMenu();
        }

        ImGui::Render();
        renderImGuiDrawData(ImGui::GetDrawData());
    }

    {
        MW_PROFILE_SCOPE("Client.Render.BgfxFrame");
        bgfx::frame();
    }
}

void RenderContext::captureMouse() {
    if (!window_) {
        return;
    }
    mouseCaptured_ = true;
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    hasMousePosition_ = false;
}

void RenderContext::releaseMouse() {
    if (!window_) {
        return;
    }
    mouseCaptured_ = false;
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    hasMousePosition_ = false;
}

void RenderContext::closeInGameMenu() {
    inGameMenuOpen_ = false;
    pendingInGameMenuAction_ = InGameMenuAction::None;
    captureMouse();
}

RenderContext::InGameMenuAction RenderContext::consumeInGameMenuAction() {
    const InGameMenuAction action = pendingInGameMenuAction_;
    pendingInGameMenuAction_ = InGameMenuAction::None;
    return action;
}

void RenderContext::invalidateChunkCache(glm::ivec3 chunkPos) {
    MW_PROFILE_COUNTER("Client.Render.CacheInvalidations", 1);

    static const std::array<glm::ivec3, 7> kInvalidateOffsets = {{glm::ivec3(0, 0, 0), glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0),
                                                                  glm::ivec3(0, 1, 0), glm::ivec3(0, -1, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1)}};
    for (const auto& off : kInvalidateOffsets) {
        chunkMeshCache_.invalidate(chunkPos + off);
    }
}

bool RenderContext::loadShaders() {
    const char* rendererDir = shaderDirectoryForRenderer(bgfx::getRendererType());
    std::filesystem::path shaderDir = std::filesystem::path("shaders") / rendererDir;
    std::vector<uint8_t> vertexShaderData = readBinaryFile(shaderDir / "vs_color.sc.bin");
    std::vector<uint8_t> fragmentShaderData = readBinaryFile(shaderDir / "fs_color.sc.bin");
    if (vertexShaderData.empty() || fragmentShaderData.empty()) {
        shaderDir = std::filesystem::path("bin") / "shaders" / rendererDir;
        vertexShaderData = readBinaryFile(shaderDir / "vs_color.sc.bin");
        fragmentShaderData = readBinaryFile(shaderDir / "fs_color.sc.bin");
    }
    if (vertexShaderData.empty() || fragmentShaderData.empty()) {
        logging::error("Failed to load shaders from {}", shaderDir.string());
        return false;
    }

    bgfx::ShaderHandle vertexShader = bgfx::createShader(bgfx::copy(vertexShaderData.data(), static_cast<uint32_t>(vertexShaderData.size())));
    bgfx::ShaderHandle fragmentShader = bgfx::createShader(bgfx::copy(fragmentShaderData.data(), static_cast<uint32_t>(fragmentShaderData.size())));
    bgfx::ProgramHandle program = bgfx::createProgram(vertexShader, fragmentShader, true);
    if (!bgfx::isValid(program)) {
        logging::error("Failed to create bgfx shader program");
        return false;
    }

    worldShader_.program = program.idx;
    return true;
}

void RenderContext::destroyShaders() {
    bgfx::ProgramHandle program{worldShader_.program};
    if (bgfx::isValid(program)) {
        bgfx::destroy(program);
        worldShader_.program = bgfx::kInvalidHandle;
    }
}

bool RenderContext::initializeImGui() {
    IMGUI_CHECKVERSION();
    imguiContext_ = ImGui::CreateContext();
    if (!imguiContext_) {
        logging::error("Failed to create ImGui context");
        return false;
    }

    ImGui::SetCurrentContext(imguiContext_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    ImGui::StyleColorsDark();

    unsigned char* pixels = nullptr;
    int fontWidth = 0;
    int fontHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fontWidth, &fontHeight);
    bgfx::TextureHandle fontTexture = bgfx::createTexture2D(
        static_cast<uint16_t>(fontWidth),
        static_cast<uint16_t>(fontHeight),
        false,
        1,
        bgfx::TextureFormat::RGBA8,
        0,
        bgfx::copy(pixels, static_cast<uint32_t>(fontWidth * fontHeight * 4)));
    if (!bgfx::isValid(fontTexture)) {
        logging::error("Failed to create ImGui font texture");
        shutdownImGui();
        return false;
    }
    imguiShader_.fontTexture = fontTexture.idx;

    bgfx::UniformHandle textureUniform = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
    if (!bgfx::isValid(textureUniform)) {
        logging::error("Failed to create ImGui texture uniform");
        shutdownImGui();
        return false;
    }
    imguiShader_.textureUniform = textureUniform.idx;

    const char* rendererDir = shaderDirectoryForRenderer(bgfx::getRendererType());
    std::filesystem::path shaderDir = std::filesystem::path("shaders") / rendererDir;
    std::vector<uint8_t> vertexShaderData = readBinaryFile(shaderDir / "vs_imgui.sc.bin");
    std::vector<uint8_t> fragmentShaderData = readBinaryFile(shaderDir / "fs_imgui.sc.bin");
    if (vertexShaderData.empty() || fragmentShaderData.empty()) {
        shaderDir = std::filesystem::path("bin") / "shaders" / rendererDir;
        vertexShaderData = readBinaryFile(shaderDir / "vs_imgui.sc.bin");
        fragmentShaderData = readBinaryFile(shaderDir / "fs_imgui.sc.bin");
    }
    if (vertexShaderData.empty() || fragmentShaderData.empty()) {
        logging::error("Failed to load ImGui shaders from {}", shaderDir.string());
        shutdownImGui();
        return false;
    }

    bgfx::ShaderHandle vertexShader = bgfx::createShader(bgfx::copy(vertexShaderData.data(), static_cast<uint32_t>(vertexShaderData.size())));
    bgfx::ShaderHandle fragmentShader = bgfx::createShader(bgfx::copy(fragmentShaderData.data(), static_cast<uint32_t>(fragmentShaderData.size())));
    bgfx::ProgramHandle program = bgfx::createProgram(vertexShader, fragmentShader, true);
    if (!bgfx::isValid(program)) {
        logging::error("Failed to create ImGui shader program");
        shutdownImGui();
        return false;
    }
    imguiShader_.program = program.idx;
    return true;
}

void RenderContext::shutdownImGui() {
    bgfx::ProgramHandle program{imguiShader_.program};
    if (bgfx::isValid(program)) {
        bgfx::destroy(program);
        imguiShader_.program = bgfx::kInvalidHandle;
    }

    bgfx::UniformHandle textureUniform{imguiShader_.textureUniform};
    if (bgfx::isValid(textureUniform)) {
        bgfx::destroy(textureUniform);
        imguiShader_.textureUniform = bgfx::kInvalidHandle;
    }

    bgfx::TextureHandle fontTexture{imguiShader_.fontTexture};
    if (bgfx::isValid(fontTexture)) {
        bgfx::destroy(fontTexture);
        imguiShader_.fontTexture = bgfx::kInvalidHandle;
    }

    if (imguiContext_) {
        ImGui::SetCurrentContext(imguiContext_);
        ImGui::DestroyContext(imguiContext_);
        imguiContext_ = nullptr;
    }
}

void RenderContext::renderWorld(const ClientWorld& world) {
    MW_PROFILE_SCOPE("Client.Render.World");

    const VoxelWorld& voxelWorld = world.getVoxelWorld();
    const auto loadedChunks = voxelWorld.getLoadedChunks();
    MW_PROFILE_GAUGE("Render.LoadedChunks", static_cast<double>(loadedChunks.size()));

    chunkMeshCache_.evictStale(loadedChunks);

    {
        MW_PROFILE_SCOPE("Client.Render.World.BuildChunkMesh");

        // Build/update cached meshes per chunk (only when dirty)
        std::unordered_map<glm::ivec3, size_t> currentCounts;
        currentCounts.reserve(loadedChunks.size());
        for (const glm::ivec3& chunkPos : loadedChunks) {
            currentCounts[chunkPos] = voxelWorld.getChunk(chunkPos).getBlockCount();
        }

        for (const glm::ivec3& chunkPos : loadedChunks) {
            const size_t blockCount = currentCounts[chunkPos];
            if (chunkMeshCache_.needsRebuild(chunkPos, blockCount, currentCounts)) {
                ChunkMeshCache::Entry entry;
                buildChunkMesh(world, chunkPos, entry);
                MW_PROFILE_COUNTER("Client.Render.MeshRebuilds", 1);
                MW_PROFILE_COUNTER("Client.Render.MeshBuildVertices", static_cast<int64_t>(entry.vertexCount));
                MW_PROFILE_COUNTER("Client.Render.MeshBuildIndices", static_cast<int64_t>(entry.indices.size()));
                chunkMeshCache_.put(chunkPos, blockCount, std::move(entry));
            }
        }
        MW_PROFILE_GAUGE("Render.MeshCacheSize", static_cast<double>(chunkMeshCache_.size()));
    }

    std::unordered_set<glm::ivec3> visibleChunks;
    {
        MW_PROFILE_SCOPE("Client.Render.World.ChunkCulling");

        // Frustum culling
        const glm::vec3 camTarget = cameraPosition_ + forward();
        float viewMat[16], projMat[16], vpMat[16];
        bx::mtxLookAt(viewMat,
                      bx::Vec3(cameraPosition_.x, cameraPosition_.y, cameraPosition_.z),
                      bx::Vec3(camTarget.x, camTarget.y, camTarget.z),
                      bx::Vec3(0.0f, 1.0f, 0.0f),
                      bx::Handedness::Right);
        bx::mtxProj(projMat, 70.0f,
                    static_cast<float>(framebufferWidth_) / static_cast<float>(framebufferHeight_),
                    0.1f, 500.0f,
                    bgfx::getCaps()->homogeneousDepth,
                    bx::Handedness::Right);
        bx::mtxMul(vpMat, viewMat, projMat);
        const Frustum frustum = Frustum::fromBxMatrix(vpMat);

        // Occlusion culling
        const glm::ivec3 cameraChunk = Chunk::worldToChunk(glm::ivec3(glm::floor(cameraPosition_)));

        std::unordered_set<glm::ivec3> loadedSet(loadedChunks.begin(), loadedChunks.end());

        struct BfsNode {
            glm::ivec3 pos;
            int inFace;
        };
        std::queue<BfsNode> bfsQueue;
        std::unordered_map<glm::ivec3, uint8_t> visitedFaces;

        auto enqueueChunk = [&](glm::ivec3 pos, int inFace) {
            if (!loadedSet.count(pos)) return;
            uint8_t bit = (inFace < 0) ? 0x40u : static_cast<uint8_t>(1u << inFace);
            uint8_t& seen = visitedFaces[pos];
            if (seen & bit) return;
            seen |= bit;
            bfsQueue.push({pos, inFace});
        };

        enqueueChunk(cameraChunk, -1);

        const float chunkWorldSize = static_cast<float>(Chunk::SIZE);

        while (!bfsQueue.empty()) {
            const BfsNode node = bfsQueue.front();
            bfsQueue.pop();

            const glm::vec3 chunkMin = glm::vec3(node.pos) * chunkWorldSize;
            const glm::vec3 chunkMax = chunkMin + glm::vec3(chunkWorldSize);
            if (!frustum.testAABB(chunkMin, chunkMax)) continue;

            visibleChunks.insert(node.pos);

            ChunkFaceConnectivity conn = ~0u;
            if (const ChunkMeshCache::Entry* e = chunkMeshCache_.get(node.pos)) {
                conn = e->faceConnectivity;
            }

            for (int outFace = 0; outFace < 6; ++outFace) {
                if (node.inFace >= 0 && !faceConnected(conn, node.inFace, outFace)) continue;
                enqueueChunk(node.pos + kFaceDir[outFace], kOppositeFace[outFace]);
            }
        }

        MW_PROFILE_GAUGE("Render.ChunksVisible", static_cast<double>(visibleChunks.size()));
        MW_PROFILE_GAUGE("Render.ChunksCulled", static_cast<double>(loadedChunks.size()) - static_cast<double>(visibleChunks.size()));
    }

    MeshBuilder currentBatch;
    currentBatch.vertices.reserve(8192);
    currentBatch.indices.reserve(12288);

    {
        MW_PROFILE_SCOPE("Client.Render.World.SubmitChunkMesh");

        for (const glm::ivec3& chunkPos : loadedChunks) {
            if (!visibleChunks.count(chunkPos)) continue;

            const ChunkMeshCache::Entry* cached = chunkMeshCache_.get(chunkPos);
            if (!cached || cached->vertexCount == 0 || cached->indices.empty()) {
                continue;
            }

            if (cached->vertexCount > kMaxBatchVertices || cached->indices.size() > kMaxBatchIndices) {
                continue;
            }

            if (currentBatch.vertices.size() + cached->vertexCount > kMaxBatchVertices ||
                currentBatch.indices.size() + cached->indices.size() > kMaxBatchIndices) {
                submitMeshBatch(currentBatch, worldShader_.program);
                currentBatch.vertices.clear();
                currentBatch.indices.clear();
            }

            const auto baseVertex = static_cast<uint16_t>(currentBatch.vertices.size());
            constexpr size_t kFloatsPerVertex = 4;
            for (size_t i = 0; i < cached->vertexCount; ++i) {
                float x = cached->vertexData[i * kFloatsPerVertex + 0];
                float y = cached->vertexData[i * kFloatsPerVertex + 1];
                float z = cached->vertexData[i * kFloatsPerVertex + 2];
                uint32_t abgr;
                std::memcpy(&abgr, &cached->vertexData[i * kFloatsPerVertex + 3], sizeof(uint32_t));
                currentBatch.vertices.push_back(PosColorVertex{x, y, z, abgr});
            }
            for (uint16_t idx : cached->indices) {
                currentBatch.indices.push_back(baseVertex + idx);
            }
        }

        submitMeshBatch(currentBatch, worldShader_.program);
        currentBatch.vertices.clear();
        currentBatch.indices.clear();
    }

    {
        MW_PROFILE_SCOPE("Client.Render.World.Entities");

        const auto& registry = world.getActorWorld().registry();
        auto viewEntities = registry.view<TransformComponent, MeshComponent>();
        MW_PROFILE_GAUGE("Render.VisibleEntities", static_cast<double>(viewEntities.size_hint()));
        for (auto entity : viewEntities) {
            const auto& meshComp = registry.get<MeshComponent>(entity);
            if (!meshComp.isVisible || shouldHideLocalPlayerModel(world, entity)) {
                continue;
            }

            const bool actorModel = registry.all_of<PlayerComponent>(entity) || registry.all_of<RobotComponent>(entity);
            const size_t requiredVertices = actorModel ? kPlayerModelVertexCount : kBoxVertexCount;
            const size_t requiredIndices = actorModel ? kPlayerModelIndexCount : kBoxIndexCount;
            if (currentBatch.vertices.size() + requiredVertices > kMaxBatchVertices ||
                currentBatch.indices.size() + requiredIndices > kMaxBatchIndices) {
                submitMeshBatch(currentBatch, worldShader_.program);
                currentBatch.vertices.clear();
                currentBatch.indices.clear();
            }
            const auto& transform = registry.get<TransformComponent>(entity);
            const glm::vec3 color(meshComp.color.r, meshComp.color.g, meshComp.color.b);
            if (actorModel) {
                addPlayerModel(currentBatch, transform, color);
            } else {
                const glm::vec3 center = transform.position + glm::vec3(0.0f, 0.91f, 0.0f);
                addOrientedBox(currentBatch, center, glm::vec3(0.35f, 0.90f, 0.35f), transform.rotation.y, color);
            }
        }

        submitMeshBatch(currentBatch, worldShader_.program);
    }

    {
        MW_PROFILE_SCOPE("Client.Render.World.ChunkBounds");

        if (showChunkBounds_) {
            MeshBuilder lineBatch;
            const glm::vec3 boundColor(1.0f, 0.92f, 0.25f);
            for (const glm::ivec3& chunkPos : loadedChunks) {
                const glm::vec3 min = glm::vec3(chunkPos) * static_cast<float>(Chunk::SIZE);
                const glm::vec3 max = min + glm::vec3(static_cast<float>(Chunk::SIZE));
                addLineBox(lineBatch, min, max, boundColor);
            }
            submitLineBatch(lineBatch, worldShader_.program);
        }
    }
}

void RenderContext::renderProfilerOverlay() {
    MW_PROFILE_SCOPE("Client.Render.Profiler");

    const profiling::Snapshot snapshot = profiling::Profiler::instance().snapshot();

    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.82f);
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;

    constexpr float kColName = 220.0f;
    constexpr float kVisibleTableRows = 8.0f;
    constexpr ImGuiTableFlags kProfilerTableFlags = ImGuiTableFlags_SizingStretchSame |
                                                    ImGuiTableFlags_BordersInnerV |
                                                    ImGuiTableFlags_ScrollY;
    const float tableHeight = ImGui::GetTextLineHeightWithSpacing() * (kVisibleTableRows + 1.0f);

    auto rightAlignedText = [](const char* fmt, auto&&... args) {
        char text[128];
        std::snprintf(text, sizeof(text), fmt, std::forward<decltype(args)>(args)...);
        const float textWidth = ImGui::CalcTextSize(text).x;
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        if (availableWidth > textWidth) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availableWidth - textWidth));
        }
        ImGui::TextUnformatted(text);
    };

    if (ImGui::Begin("ProfilerOverlay", nullptr, flags)) {
        char buffer[128];
        glm::ivec3 chunkCoord = Chunk::worldToChunk(glm::ivec3(
            static_cast<int>(std::floor(cameraPosition_.x)),
            static_cast<int>(std::floor(cameraPosition_.y)),
            static_cast<int>(std::floor(cameraPosition_.z))));
        if (ImGui::BeginTable("ProfilerSummaryTop", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Camera", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Chunk", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            std::snprintf(buffer, sizeof(buffer), "%.1f, %.1f, %.1f", cameraPosition_.x, cameraPosition_.y, cameraPosition_.z);
            rightAlignedText("%s", buffer);
            ImGui::TableNextColumn();
            std::snprintf(buffer, sizeof(buffer), "%d, %d, %d", chunkCoord.x, chunkCoord.y, chunkCoord.z);
            rightAlignedText("%s", buffer);
            ImGui::EndTable();
        }

        if (ImGui::BeginTable("ProfilerSummaryBottom", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Frame #", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("FPS", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Frame Time", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Renderer", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            rightAlignedText("%llu", static_cast<unsigned long long>(snapshot.frameIndex));
            ImGui::TableNextColumn();
            rightAlignedText("%.1f", snapshot.fps);
            ImGui::TableNextColumn();
            rightAlignedText("%.1f ms", snapshot.frameMs);
            ImGui::TableNextColumn();
            rightAlignedText("%s", bgfx::getRendererName(bgfx::getRendererType()));
            ImGui::EndTable();
        }

        if (profilerMode_ == ProfilerMode::Full && ImGui::BeginTable("ProfilerScopes", 4, kProfilerTableFlags, ImVec2(0.0f, tableHeight))) {
            ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthFixed, kColName);
            ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            for (const profiling::ScopeEntry& entry : snapshot.scopes) {
                if (entry.name == "Frame.Total") {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.name.c_str());
                ImGui::TableNextColumn();
                rightAlignedText("%.1f", entry.lastMs);
                ImGui::TableNextColumn();
                rightAlignedText("%.1f", entry.avgMs);
                ImGui::TableNextColumn();
                rightAlignedText("%.1f", entry.maxMs);
            }
            ImGui::EndTable();
        }

        if (profilerMode_ == ProfilerMode::Full && ImGui::BeginTable("ProfilerCounters", 4, kProfilerTableFlags, ImVec2(0.0f, tableHeight))) {
            ImGui::TableSetupColumn("Counter", ImGuiTableColumnFlags_WidthFixed, kColName);
            ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            for (const profiling::CounterEntry& entry : snapshot.counters) {
                if (entry.lastValue == 0 && entry.totalValue == 0) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.name.c_str());
                ImGui::TableNextColumn();
                rightAlignedText("%lld", static_cast<long long>(entry.lastValue));
                ImGui::TableNextColumn();
                rightAlignedText("%.1f", entry.avgValue);
                ImGui::TableNextColumn();
                rightAlignedText("%lld", static_cast<long long>(entry.maxValue));
            }
            ImGui::EndTable();
        }

        if (profilerMode_ == ProfilerMode::Full && ImGui::BeginTable("ProfilerGauges", 4, kProfilerTableFlags, ImVec2(0.0f, tableHeight))) {
            ImGui::TableSetupColumn("Gauge", ImGuiTableColumnFlags_WidthFixed, kColName);
            ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            for (const profiling::GaugeEntry& entry : snapshot.gauges) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.name.c_str());
                ImGui::TableNextColumn();
                rightAlignedText("%.1f", entry.value);
                ImGui::TableNextColumn();
                rightAlignedText("%.1f", entry.avgValue);
                ImGui::TableNextColumn();
                rightAlignedText("%.1f", entry.maxValue);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void RenderContext::renderInGameMenu() {
    MW_PROFILE_SCOPE("Client.Render.Menu");

    ImGui::SetNextWindowPos(ImVec2(windowWidth_ * 0.5f, windowHeight_ * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(260.0f, 0.0f), ImGuiCond_Always);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Game Menu", nullptr, flags)) {
        if (ImGui::Button("Resume", ImVec2(-1.0f, 36.0f))) {
            closeInGameMenu();
        }
        if (ImGui::Button("Exit to Start", ImVec2(-1.0f, 36.0f))) {
            pendingInGameMenuAction_ = InGameMenuAction::ReturnToStart;
        }
    }
    ImGui::End();
}

void RenderContext::renderCursorOverlay() {
    MW_PROFILE_SCOPE("Client.Render.Cursor");
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const ImVec2 center(static_cast<float>(windowWidth_) * 0.5f, static_cast<float>(windowHeight_) * 0.5f);

    if (cursorMode_ == CursorMode::Cross) {
        drawList->AddLine(ImVec2(center.x - 12.0f, center.y), ImVec2(center.x + 12.0f, center.y), IM_COL32(255, 255, 255, 255), 2.0f);
        drawList->AddLine(ImVec2(center.x, center.y - 12.0f), ImVec2(center.x, center.y + 12.0f), IM_COL32(255, 255, 255, 255), 2.0f);
    } else if (cursorMode_ == CursorMode::XYZ) {
        const glm::vec3 cameraRight = right();
        const glm::vec3 cameraForward = forward();
        const glm::vec3 cameraUp = glm::normalize(glm::cross(cameraRight, cameraForward));

        const glm::vec3 xAxis(1.0f, 0.0f, 0.0f);
        const glm::vec3 yAxis(0.0f, 1.0f, 0.0f);
        const glm::vec3 zAxis(0.0f, 0.0f, 1.0f);

        const float scale = 18.0f;
        const glm::vec2 xDir = glm::vec2(glm::dot(xAxis, cameraRight), glm::dot(xAxis, cameraUp)) * scale;
        const glm::vec2 yDir = glm::vec2(glm::dot(yAxis, cameraRight), glm::dot(yAxis, cameraUp)) * scale;
        const glm::vec2 zDir = glm::vec2(glm::dot(zAxis, cameraRight), glm::dot(zAxis, cameraUp)) * scale;

        const ImVec2 xEnd(center.x + xDir.x, center.y - xDir.y);
        const ImVec2 yEnd(center.x + yDir.x, center.y - yDir.y);
        const ImVec2 zEnd(center.x + zDir.x, center.y - zDir.y);

        drawList->AddLine(center, xEnd, IM_COL32(220, 80, 80, 255), 2.5f);
        drawList->AddLine(center, yEnd, IM_COL32(100, 220, 100, 255), 2.5f);
        drawList->AddLine(center, zEnd, IM_COL32(100, 140, 220, 255), 2.5f);
        drawList->AddText(ImVec2(xEnd.x + 4.0f, xEnd.y - 6.0f), IM_COL32(220, 80, 80, 255), "X");
        drawList->AddText(ImVec2(yEnd.x + 4.0f, yEnd.y - 6.0f), IM_COL32(100, 220, 100, 255), "Y");
        drawList->AddText(ImVec2(zEnd.x + 4.0f, zEnd.y - 6.0f), IM_COL32(100, 140, 220, 255), "Z");
    }
}

void RenderContext::renderImGuiDrawData(ImDrawData* drawData) {
    if (!drawData || drawData->CmdListsCount == 0) {
        return;
    }

    const int framebufferWidth = static_cast<int>(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    const int framebufferHeight = static_cast<int>(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (framebufferWidth <= 0 || framebufferHeight <= 0) {
        return;
    }

    bgfx::setViewMode(kImGuiView, bgfx::ViewMode::Sequential);
    bgfx::setViewRect(kImGuiView, 0, 0, static_cast<uint16_t>(framebufferWidth), static_cast<uint16_t>(framebufferHeight));

    float projection[16];
    bx::mtxOrtho(
        projection,
        drawData->DisplayPos.x,
        drawData->DisplayPos.x + drawData->DisplaySize.x,
        drawData->DisplayPos.y + drawData->DisplaySize.y,
        drawData->DisplayPos.y,
        0.0f,
        1000.0f,
        0.0f,
        bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewTransform(kImGuiView, nullptr, projection);
    bgfx::touch(kImGuiView);

    const bgfx::ProgramHandle program{imguiShader_.program};
    const bgfx::TextureHandle fontTexture{imguiShader_.fontTexture};
    const bgfx::UniformHandle textureUniform{imguiShader_.textureUniform};
    if (!bgfx::isValid(program) || !bgfx::isValid(fontTexture) || !bgfx::isValid(textureUniform)) {
        return;
    }

    const ImVec2 clipOffset = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;

    for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex) {
        const ImDrawList* cmdList = drawData->CmdLists[listIndex];
        const uint32_t vertexCount = static_cast<uint32_t>(cmdList->VtxBuffer.Size);
        const uint32_t indexCount = static_cast<uint32_t>(cmdList->IdxBuffer.Size);
        if (vertexCount == 0 || indexCount == 0) {
            continue;
        }
        if (bgfx::getAvailTransientVertexBuffer(vertexCount, ImGuiVertex::layout) < vertexCount ||
            bgfx::getAvailTransientIndexBuffer(indexCount, sizeof(ImDrawIdx) == 4) < indexCount) {
            break;
        }

        bgfx::TransientVertexBuffer vertexBuffer;
        bgfx::TransientIndexBuffer indexBuffer;
        bgfx::allocTransientVertexBuffer(&vertexBuffer, vertexCount, ImGuiVertex::layout);
        bgfx::allocTransientIndexBuffer(&indexBuffer, indexCount, sizeof(ImDrawIdx) == 4);
        std::memcpy(vertexBuffer.data, cmdList->VtxBuffer.Data, vertexCount * sizeof(ImDrawVert));
        std::memcpy(indexBuffer.data, cmdList->IdxBuffer.Data, indexCount * sizeof(ImDrawIdx));

        for (const ImDrawCmd& command : cmdList->CmdBuffer) {
            if (command.UserCallback) {
                command.UserCallback(cmdList, &command);
                continue;
            }

            ImVec4 clipRect;
            clipRect.x = (command.ClipRect.x - clipOffset.x) * clipScale.x;
            clipRect.y = (command.ClipRect.y - clipOffset.y) * clipScale.y;
            clipRect.z = (command.ClipRect.z - clipOffset.x) * clipScale.x;
            clipRect.w = (command.ClipRect.w - clipOffset.y) * clipScale.y;
            if (clipRect.x >= framebufferWidth || clipRect.y >= framebufferHeight || clipRect.z < 0.0f || clipRect.w < 0.0f) {
                continue;
            }

            const float scissorX1 = std::clamp(clipRect.x, 0.0f, static_cast<float>(framebufferWidth));
            const float scissorY1 = std::clamp(clipRect.y, 0.0f, static_cast<float>(framebufferHeight));
            const float scissorX2 = std::clamp(clipRect.z, 0.0f, static_cast<float>(framebufferWidth));
            const float scissorY2 = std::clamp(clipRect.w, 0.0f, static_cast<float>(framebufferHeight));
            const uint16_t scissorX = static_cast<uint16_t>(scissorX1);
            const uint16_t scissorY = static_cast<uint16_t>(scissorY1);
            const uint16_t scissorW = static_cast<uint16_t>(std::max(scissorX2 - scissorX1, 0.0f));
            const uint16_t scissorH = static_cast<uint16_t>(std::max(scissorY2 - scissorY1, 0.0f));

            bgfx::setScissor(scissorX, scissorY, scissorW, scissorH);
            bgfx::setTexture(0, textureUniform, fontTexture);
            bgfx::setVertexBuffer(0, &vertexBuffer, command.VtxOffset, vertexCount - command.VtxOffset);
            bgfx::setIndexBuffer(&indexBuffer, command.IdxOffset, command.ElemCount);
            bgfx::setState(
                BGFX_STATE_WRITE_RGB |
                BGFX_STATE_WRITE_A |
                BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA) |
                BGFX_STATE_MSAA);
            bgfx::submit(kImGuiView, program);
        }
    }
}

void RenderContext::buildChunkMesh(const ClientWorld& world, glm::ivec3 chunkPos, ChunkMeshCache::Entry& outMesh) {
    const VoxelWorld& voxelWorld = world.getVoxelWorld();
    const Chunk& chunk = voxelWorld.getChunk(chunkPos);

    std::vector<PosColorVertex> vertices;
    std::vector<uint16_t> indices;
    vertices.reserve(1024);
    indices.reserve(1536);

    for (int x = 0; x < Chunk::SIZE; ++x) {
        for (int y = 0; y < Chunk::SIZE; ++y) {
            for (int z = 0; z < Chunk::SIZE; ++z) {
                const glm::ivec3 localPos(x, y, z);
                const BlockData block = chunk.getBlock(localPos);
                if (block.type == BlockType::Air) {
                    continue;
                }

                const glm::ivec3 worldPos = chunk.localToWorld(localPos);
                const glm::vec3 baseColor = blockColor(block.type);
                for (const Face& face : kFaces) {
                    // Face culling
                    if (world.getBlock(worldPos + face.normal).type != BlockType::Air) {
                        continue;
                    }

                    if (vertices.size() > kMaxBatchVertices - 4 || indices.size() > kMaxBatchIndices - 6) {
                        goto done;
                    }

                    const auto start = static_cast<uint16_t>(vertices.size());
                    const uint32_t packedColor = packColor(baseColor * face.shade);
                    for (const glm::vec3& corner : face.corners) {
                        glm::vec3 pos = glm::vec3(worldPos) + corner;
                        vertices.push_back(PosColorVertex{pos.x, pos.y, pos.z, packedColor});
                    }
                    indices.push_back(start + 0);
                    indices.push_back(start + 1);
                    indices.push_back(start + 2);
                    indices.push_back(start + 0);
                    indices.push_back(start + 2);
                    indices.push_back(start + 3);
                }
            }
        }
    }
done:

    outMesh.vertexCount = vertices.size();
    outMesh.vertexData.resize(vertices.size() * 4);
    for (size_t i = 0; i < vertices.size(); ++i) {
        outMesh.vertexData[i * 4 + 0] = vertices[i].x;
        outMesh.vertexData[i * 4 + 1] = vertices[i].y;
        outMesh.vertexData[i * 4 + 2] = vertices[i].z;
        float colorAsFloat;
        std::memcpy(&colorAsFloat, &vertices[i].abgr, sizeof(float));
        outMesh.vertexData[i * 4 + 3] = colorAsFloat;
    }
    outMesh.indices = std::move(indices);
    outMesh.faceConnectivity = computeFaceConnectivity(chunk);
}

void RenderContext::updateImGuiInput() {
    ImGuiIO& io = ImGui::GetIO();
    io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
    io.MouseWheel = 0.0f;

    if (!window_ || mouseCaptured_) {
        imguiScrollY_ = 0.0;
        io.MouseDown[0] = false;
        io.MouseDown[1] = false;
        io.MouseDown[2] = false;
        return;
    }

    double mouseX = 0.0;
    double mouseY = 0.0;
    glfwGetCursorPos(window_, &mouseX, &mouseY);
    io.MousePos = ImVec2(static_cast<float>(mouseX), static_cast<float>(mouseY));
    io.MouseDown[0] = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    io.MouseDown[1] = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    io.MouseDown[2] = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;

    if (imguiScrollY_ != 0.0) {
        io.MouseWheel = static_cast<float>(imguiScrollY_);
        imguiScrollY_ = 0.0;
    }
}

void RenderContext::handleScroll(GLFWwindow* window, double, double yOffset) {
    auto* renderContext = static_cast<RenderContext*>(glfwGetWindowUserPointer(window));
    if (!renderContext || renderContext->mouseCaptured_) {
        return;
    }

    renderContext->imguiScrollY_ += yOffset;
}

glm::vec3 RenderContext::forward() const {
    const float yaw = glm::radians(cameraYaw_);
    const float pitch = glm::radians(cameraPitch_);
    return glm::normalize(glm::vec3(
        std::cos(yaw) * std::cos(pitch),
        std::sin(pitch),
        std::sin(yaw) * std::cos(pitch)));
}

glm::vec3 RenderContext::right() const {
    return glm::normalize(glm::cross(forward(), glm::vec3(0.0f, 1.0f, 0.0f)));
}

bool RenderContext::shouldHideLocalPlayerModel(const ClientWorld& world, entt::entity entity) const {
    if (cameraViewMode_ != CameraViewMode::FirstPerson) {
        return false;
    }

    const auto& registry = world.getActorWorld().registry();
    if (!registry.all_of<SessionComponent, PlayerComponent>(entity)) {
        return false;
    }

    const auto& session = registry.get<SessionComponent>(entity);
    const auto& player = registry.get<PlayerComponent>(entity);
    return session.sessionId == localSessionId_ && player.mode == PlayerMode::Survival;
}
