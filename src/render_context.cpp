#include "render_context.h"

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/math.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <queue>
#include <unordered_set>
#include <vector>

#include "actor_world.h"
#include "chunk_layout.h"
#include "chunk_mesh.h"
#include "chunk_mesh_pool.h"
#include "client_chunk_manager.h"
#include "config.h"
#include "entity.h"
#include "helper.h"
#include "log.h"
#include "profiler.h"
#include "voxel_world.h"

namespace {

template <typename T>
T cycleMode(T current) {
    return static_cast<T>((static_cast<int>(current) + 1) % static_cast<int>(T::Count));
}

constexpr bgfx::ViewId kMainView = 0;
constexpr bgfx::ViewId kImGuiView = 1;
constexpr uint32_t kDepthLast = UINT32_MAX;
constexpr size_t kBoxVertexCount = 24;
constexpr size_t kPlayerModelVertexCount = kBoxVertexCount * 2;
constexpr size_t kLineBoxVertexCount = 8;
constexpr size_t kMaxBatchVertices = UINT16_MAX;

uint32_t depthSortKey(float distanceSq) {
    return bx::floatToBits(distanceSq);
}

uint32_t bgfxResetFlags() {
    return AppConfig::instance().vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;
}

template <typename Handle>
void destroyHandle(uint16_t& index) {
    Handle handle{index};
    if (bgfx::isValid(handle)) {
        bgfx::destroy(handle);
        index = bgfx::kInvalidHandle;
    }
}

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
    assert(mesh.vertices.size() + 4 <= kMaxBatchVertices);

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
    assert(mesh.vertices.size() + kLineBoxVertexCount <= kMaxBatchVertices);

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

void submitLineBatch(const MeshBuilder& mesh, unsigned short programIndex, uint32_t depth) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return;
    }
    const uint32_t vertexCount = static_cast<uint32_t>(mesh.vertices.size());
    const uint32_t indexCount = static_cast<uint32_t>(mesh.indices.size());
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
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES);
        bgfx::submit(kMainView, bgfx::ProgramHandle{programIndex}, depth);
        MW_PROFILE_COUNTER("Render.LineSubmits", 1);
        MW_PROFILE_COUNTER("Render.LineVertices", static_cast<int64_t>(vertexCount));
        MW_PROFILE_COUNTER("Render.LineIndices", static_cast<int64_t>(indexCount));
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

void submitMeshBatch(const MeshBuilder& mesh, unsigned short programIndex, uint32_t depth) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return;
    }
    const uint32_t vertexCount = static_cast<uint32_t>(mesh.vertices.size());
    const uint32_t indexCount = static_cast<uint32_t>(mesh.indices.size());
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
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CW);
        bgfx::submit(kMainView, bgfx::ProgramHandle{programIndex}, depth);
        MW_PROFILE_COUNTER("Render.MeshSubmits", 1);
        MW_PROFILE_COUNTER("Render.MeshVertices", static_cast<int64_t>(vertexCount));
        MW_PROFILE_COUNTER("Render.MeshIndices", static_cast<int64_t>(indexCount));
    }
}

}  // namespace

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
    init.resolution.reset = bgfxResetFlags();
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
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

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

void RenderContext::updateDisplayMetrics() {
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
        bgfx::reset(static_cast<uint32_t>(framebufferWidth), static_cast<uint32_t>(framebufferHeight), bgfxResetFlags());
    }

    windowWidth_ = windowWidth;
    windowHeight_ = windowHeight;
    framebufferWidth_ = framebufferWidth;
    framebufferHeight_ = framebufferHeight;
    framebufferScaleX_ = static_cast<float>(framebufferWidth_) / static_cast<float>(windowWidth_);
    framebufferScaleY_ = static_cast<float>(framebufferHeight_) / static_cast<float>(windowHeight_);
}

RenderContext::StartMenuAction RenderContext::renderStartMenu(char* addressBuffer, size_t addressBufferSize, int& port) {
    if (!window_ || !bgfxInitialized_) {
        return StartMenuAction::None;
    }

    releaseMouse();

    updateDisplayMetrics();
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

RenderContext::ConnectingAction RenderContext::renderConnecting(const std::string& address, uint16_t port, const std::string& status, bool failed) {
    if (!window_ || !bgfxInitialized_) {
        return ConnectingAction::None;
    }

    releaseMouse();

    updateDisplayMetrics();
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
        if (ImGui::Begin("Connection", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::Text("Connecting to %s:%u", address.c_str(), static_cast<unsigned>(port));
            ImGui::TextUnformatted(status.c_str());
            ImGui::Spacing();
            if (ImGui::Button(failed ? "Back" : "Cancel", ImVec2(-1.0f, 36.0f))) {
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
        releaseMouse();
    } else if (!altHeld && !mouseCaptured_) {
        captureMouse();
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

void RenderContext::render(const VoxelWorld& voxelWorld, const ActorWorld& actorWorld, ClientChunkManager& chunkManager) {
    if (!window_ || !bgfxInitialized_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    float deltaTime = 1.0f / 60.0f;
    if (hasLastRenderTime_) {
        const std::chrono::duration<float> elapsed = now - lastRenderTime_;
        deltaTime = std::clamp(elapsed.count(), 1.0f / 1000.0f, 0.1f);
    }
    lastRenderTime_ = now;
    hasLastRenderTime_ = true;

    updateDisplayMetrics();

    bgfx::setViewMode(kMainView, bgfx::ViewMode::DepthAscending);
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

    renderWorld(voxelWorld, actorWorld, chunkManager);

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

    uint32_t frameNumber = 0;
    {
        MW_PROFILE_SCOPE("Render.BgfxFrame");
        frameNumber = bgfx::frame();
    }
    chunkManager.onFrameSubmitted(frameNumber);
}

void RenderContext::captureMouse() {
    if (!window_ || mouseCaptured_) {
        return;
    }
    mouseCaptured_ = true;
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    hasMousePosition_ = false;
}

void RenderContext::releaseMouse() {
    if (!window_ || !mouseCaptured_) {
        return;
    }
    mouseCaptured_ = false;
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    hasMousePosition_ = false;
}

void RenderContext::resetInGameMenu() {
    inGameMenuOpen_ = false;
    pendingInGameMenuAction_ = InGameMenuAction::None;
}

RenderContext::InGameMenuAction RenderContext::consumeInGameMenuAction() {
    const InGameMenuAction action = pendingInGameMenuAction_;
    pendingInGameMenuAction_ = InGameMenuAction::None;
    return action;
}

bool RenderContext::loadProgram(const char* vertexName, const char* fragmentName, uint16_t& program) {
    const char* rendererDir = shaderDirectoryForRenderer(bgfx::getRendererType());
    std::filesystem::path shaderDir = std::filesystem::path("shaders") / rendererDir;
    std::vector<uint8_t> vertexShaderData = readBinaryFile(shaderDir / vertexName);
    std::vector<uint8_t> fragmentShaderData = readBinaryFile(shaderDir / fragmentName);
    if (vertexShaderData.empty() || fragmentShaderData.empty()) {
        shaderDir = std::filesystem::path("bin") / "shaders" / rendererDir;
        vertexShaderData = readBinaryFile(shaderDir / vertexName);
        fragmentShaderData = readBinaryFile(shaderDir / fragmentName);
    }
    if (vertexShaderData.empty() || fragmentShaderData.empty()) {
        logging::error("Failed to load shaders {}/{} from {}", vertexName, fragmentName, shaderDir.string());
        return false;
    }

    const bgfx::ShaderHandle vertexShader = bgfx::createShader(bgfx::copy(vertexShaderData.data(), static_cast<uint32_t>(vertexShaderData.size())));
    const bgfx::ShaderHandle fragmentShader = bgfx::createShader(bgfx::copy(fragmentShaderData.data(), static_cast<uint32_t>(fragmentShaderData.size())));
    if (!bgfx::isValid(vertexShader) || !bgfx::isValid(fragmentShader)) {
        logging::error("Failed to create bgfx shader {}", bgfx::isValid(vertexShader) ? fragmentName : vertexName);
        if (bgfx::isValid(vertexShader)) {
            bgfx::destroy(vertexShader);
        }
        if (bgfx::isValid(fragmentShader)) {
            bgfx::destroy(fragmentShader);
        }
        return false;
    }

    const bgfx::ProgramHandle handle = bgfx::createProgram(vertexShader, fragmentShader, true);
    if (!bgfx::isValid(handle)) {
        logging::error("Failed to create bgfx shader program {}/{}", vertexName, fragmentName);
        bgfx::destroy(vertexShader);
        bgfx::destroy(fragmentShader);
        return false;
    }

    program = handle.idx;
    return true;
}

bool RenderContext::loadShaders() {
    if (!loadProgram("vs_unlit.sc.bin", "fs_unlit.sc.bin", unlitShader_.program)) {
        return false;
    }
    if (!loadProgram("vs_chunk.sc.bin", "fs_chunk.sc.bin", chunkShader_.program)) {
        return false;
    }
    return true;
}

void RenderContext::destroyShaders() {
    destroyHandle<bgfx::ProgramHandle>(unlitShader_.program);
    destroyHandle<bgfx::ProgramHandle>(chunkShader_.program);
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

    if (!loadProgram("vs_imgui.sc.bin", "fs_imgui.sc.bin", imguiShader_.program)) {
        shutdownImGui();
        return false;
    }
    return true;
}

void RenderContext::shutdownImGui() {
    destroyHandle<bgfx::ProgramHandle>(imguiShader_.program);
    destroyHandle<bgfx::UniformHandle>(imguiShader_.textureUniform);
    destroyHandle<bgfx::TextureHandle>(imguiShader_.fontTexture);

    if (imguiContext_) {
        ImGui::SetCurrentContext(imguiContext_);
        ImGui::DestroyContext(imguiContext_);
        imguiContext_ = nullptr;
    }
}

void RenderContext::renderWorld(const VoxelWorld& voxelWorld, const ActorWorld& actorWorld, const ClientChunkManager& chunkManager) {
    MW_PROFILE_SCOPE("Render.World");

    std::vector<glm::ivec3> loadedChunks;
    voxelWorld.forEachLoadedChunk([&](glm::ivec3 chunkPos) {
        loadedChunks.push_back(chunkPos);
    });
    MW_PROFILE_GAUGE("Render.LoadedChunks", static_cast<double>(loadedChunks.size()));
    MW_PROFILE_GAUGE("Render.MeshCacheSize", static_cast<double>(chunkManager.meshCount()));

    std::unordered_set<glm::ivec3> visibleChunks;
    {
        MW_PROFILE_SCOPE("Render.World.ChunkCulling");

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
        const glm::ivec3 cameraChunk = ChunkLayout::worldToChunk(glm::ivec3(glm::floor(cameraPosition_)));
        const float chunkWorldSize = static_cast<float>(ChunkLayout::SIZE);

        std::unordered_set<glm::ivec3> loadedSet(loadedChunks.begin(), loadedChunks.end());

        struct BfsNode {
            glm::ivec3 pos;
            int inFace;
        };
        std::queue<BfsNode> bfsQueue;
        std::unordered_map<glm::ivec3, uint8_t> visitedFaces;
        std::unordered_set<glm::ivec3> reachableChunks;

        auto enqueueChunk = [&](glm::ivec3 pos, int inFace) {
            if (!loadedSet.count(pos)) return;
            uint8_t bit = (inFace < 0) ? 0x40u : static_cast<uint8_t>(1u << inFace);
            uint8_t& seen = visitedFaces[pos];
            if (seen & bit) return;
            seen |= bit;
            bfsQueue.push({pos, inFace});
        };

        if (loadedSet.count(cameraChunk)) {
            enqueueChunk(cameraChunk, -1);
        } else {
            for (const glm::ivec3& chunkPos : loadedChunks) {
                const glm::vec3 chunkMin = glm::vec3(chunkPos) * chunkWorldSize;
                const glm::vec3 chunkMax = chunkMin + glm::vec3(chunkWorldSize);
                if (frustum.testAABB(chunkMin, chunkMax)) {
                    visibleChunks.insert(chunkPos);
                }
            }
        }

        while (!bfsQueue.empty()) {
            const BfsNode node = bfsQueue.front();
            bfsQueue.pop();

            reachableChunks.insert(node.pos);

            const ChunkFaceConnectivity conn = chunkManager.faceConnectivity(node.pos);

            for (int outFace = 0; outFace < 6; ++outFace) {
                if (node.inFace >= 0 && !chunkFacesConnected(conn, node.inFace, outFace)) continue;
                enqueueChunk(node.pos + kChunkFaceOffsets[outFace], kOppositeChunkFace[outFace]);
            }
        }

        for (const glm::ivec3& chunkPos : reachableChunks) {
            const glm::vec3 chunkMin = glm::vec3(chunkPos) * chunkWorldSize;
            const glm::vec3 chunkMax = chunkMin + glm::vec3(chunkWorldSize);
            if (frustum.testAABB(chunkMin, chunkMax)) {
                visibleChunks.insert(chunkPos);
            }
        }

        MW_PROFILE_GAUGE("Render.ChunksVisible", static_cast<double>(visibleChunks.size()));
        MW_PROFILE_GAUGE("Render.ChunksCulled", static_cast<double>(loadedChunks.size()) - static_cast<double>(visibleChunks.size()));
    }

    {
        MW_PROFILE_SCOPE("Render.World.SubmitChunkMesh");

        const bgfx::IndexBufferHandle quadIndexBuffer{chunkManager.quadIndexBuffer()};
        if (bgfx::isValid(quadIndexBuffer)) {
            int64_t submittedChunks = 0;
            int64_t submittedVertices = 0;
            for (const glm::ivec3& chunkPos : visibleChunks) {
                const std::optional<ChunkMeshBinding> binding = chunkManager.meshBinding(chunkPos);
                if (!binding) {
                    continue;
                }
                const glm::vec3 center = (glm::vec3(chunkPos) + glm::vec3(0.5f)) * static_cast<float>(ChunkLayout::SIZE);
                const glm::vec3 offset = center - cameraPosition_;

                float model[16];
                bx::mtxTranslate(model, static_cast<float>(chunkPos.x * ChunkLayout::SIZE), static_cast<float>(chunkPos.y * ChunkLayout::SIZE), static_cast<float>(chunkPos.z * ChunkLayout::SIZE));
                bgfx::setTransform(model);
                bgfx::setVertexBuffer(0, bgfx::DynamicVertexBufferHandle{binding->vertexBuffer}, binding->vertexOffset, binding->vertexCount);
                bgfx::setIndexBuffer(quadIndexBuffer, 0, ChunkMeshPool::indexCountForVertices(binding->vertexCount));
                bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CW);
                bgfx::submit(kMainView, bgfx::ProgramHandle{chunkShader_.program}, depthSortKey(glm::dot(offset, offset)));
                ++submittedChunks;
                submittedVertices += binding->vertexCount;
            }

            MW_PROFILE_COUNTER("Render.ChunkSubmits", submittedChunks);
            MW_PROFILE_COUNTER("Render.ChunkVertices", submittedVertices);
        }

        MW_PROFILE_GAUGE("Render.ChunkPoolReservedMB", static_cast<double>(chunkManager.meshBytesReserved()) / (1024.0 * 1024.0));
        MW_PROFILE_GAUGE("Render.ChunkPoolCommittedMB", static_cast<double>(chunkManager.meshBytesCommitted()) / (1024.0 * 1024.0));
        MW_PROFILE_GAUGE("Render.ChunkPoolUsedMB", static_cast<double>(chunkManager.meshBytesUsed()) / (1024.0 * 1024.0));
    }

    {
        MW_PROFILE_SCOPE("Render.World.Entities");

        MeshBuilder entityBatch;
        entityBatch.vertices.reserve(8192);
        entityBatch.indices.reserve(12288);
        const auto& registry = actorWorld.registry();
        auto view = registry.view<TransformComponent, MeshComponent>();
        MW_PROFILE_GAUGE("Render.VisibleEntities", static_cast<double>(view.size_hint()));
        for (auto entity : view) {
            const auto& meshComp = view.get<MeshComponent>(entity);
            if (!meshComp.isVisible || shouldHideLocalPlayerModel(actorWorld, entity)) {
                continue;
            }

            const bool actorModel = registry.all_of<PlayerComponent>(entity) || registry.all_of<RobotComponent>(entity);
            const size_t requiredVertices = actorModel ? kPlayerModelVertexCount : kBoxVertexCount;
            if (entityBatch.vertices.size() + requiredVertices > kMaxBatchVertices) {
                submitMeshBatch(entityBatch, unlitShader_.program, kDepthLast);
                entityBatch.vertices.clear();
                entityBatch.indices.clear();
            }
            const auto& transform = view.get<TransformComponent>(entity);
            const glm::vec3 color(meshComp.color.r, meshComp.color.g, meshComp.color.b);
            if (actorModel) {
                addPlayerModel(entityBatch, transform, color);
            } else {
                const glm::vec3 center = transform.position + glm::vec3(0.0f, 0.91f, 0.0f);
                addOrientedBox(entityBatch, center, glm::vec3(0.35f, 0.90f, 0.35f), transform.rotation.y, color);
            }
        }

        submitMeshBatch(entityBatch, unlitShader_.program, kDepthLast);
    }

    {
        MW_PROFILE_SCOPE("Render.World.ChunkBounds");

        if (showChunkBounds_) {
            MeshBuilder lineBatch;
            const glm::vec3 boundColor(1.0f, 0.92f, 0.25f);
            for (const glm::ivec3& chunkPos : loadedChunks) {
                if (lineBatch.vertices.size() + kLineBoxVertexCount > kMaxBatchVertices) {
                    submitLineBatch(lineBatch, unlitShader_.program, kDepthLast);
                    lineBatch.vertices.clear();
                    lineBatch.indices.clear();
                }

                const glm::vec3 min = glm::vec3(chunkPos) * static_cast<float>(ChunkLayout::SIZE);
                const glm::vec3 max = min + glm::vec3(static_cast<float>(ChunkLayout::SIZE));
                addLineBox(lineBatch, min, max, boundColor);
            }
            submitLineBatch(lineBatch, unlitShader_.program, kDepthLast);
        }
    }
}

void RenderContext::renderProfilerOverlay() {
    MW_PROFILE_SCOPE("Render.Profiler");

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
        glm::ivec3 chunkCoord = ChunkLayout::worldToChunk(glm::ivec3(
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
    MW_PROFILE_SCOPE("Render.Menu");

    ImGui::SetNextWindowPos(ImVec2(windowWidth_ * 0.5f, windowHeight_ * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(260.0f, 0.0f), ImGuiCond_Always);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Game Menu", nullptr, flags)) {
        if (ImGui::Button("Resume", ImVec2(-1.0f, 36.0f))) {
            inGameMenuOpen_ = false;
            pendingInGameMenuAction_ = InGameMenuAction::None;
            captureMouse();
        }
        if (ImGui::Button("Exit to Start", ImVec2(-1.0f, 36.0f))) {
            inGameMenuOpen_ = false;
            pendingInGameMenuAction_ = InGameMenuAction::ReturnToStart;
        }
    }
    ImGui::End();
}

void RenderContext::renderCursorOverlay() {
    MW_PROFILE_SCOPE("Render.Cursor");
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
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA) | BGFX_STATE_MSAA);
            bgfx::submit(kImGuiView, program);
        }
    }
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

bool RenderContext::shouldHideLocalPlayerModel(const ActorWorld& actorWorld, entt::entity entity) const {
    if (cameraViewMode_ != CameraViewMode::FirstPerson) {
        return false;
    }

    const auto& registry = actorWorld.registry();
    if (!registry.all_of<SessionComponent>(entity)) {
        return false;
    }

    const auto& session = registry.get<SessionComponent>(entity);
    return session.sessionId == localSessionId_;
}
