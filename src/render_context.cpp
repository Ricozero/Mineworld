#include "render_context.h"

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#if defined(__linux__)
#undef None
#undef Bool
#undef Status
#undef Success
#undef Always
#endif

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/math.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <glm/packing.hpp>
#include <vector>

#include "actor_world.h"
#include "chunk_culler.h"
#include "chunk_layout.h"
#include "chunk_mesh_pool.h"
#include "client_chunk_manager.h"
#include "config.h"
#include "entity.h"
#include "log.h"
#include "profiler.h"
#include "text.h"

namespace {

template <typename T>
T cycleMode(T current) {
    return static_cast<T>((static_cast<int>(current) + 1) % static_cast<int>(T::Count));
}

constexpr bgfx::ViewId kMainView = 0;
constexpr bgfx::ViewId kImGuiView = 1;
constexpr uint32_t kDepthLast = std::numeric_limits<uint32_t>::max();
constexpr size_t kBoxVertexCount = 24;
constexpr size_t kPlayerModelVertexCount = kBoxVertexCount * 2;
constexpr size_t kLineBoxVertexCount = 8;
constexpr size_t kMaxBatchVertices = UINT16_MAX;
constexpr float kVerticalFieldOfView = 70.0f;
constexpr float kNearPlane = 0.1f;
constexpr float kFarPlane = 1000.0f;
constexpr auto kConsoleMessageDisplayDuration = std::chrono::seconds(10);
constexpr auto kConsoleMessageFadeDuration = std::chrono::seconds(2);

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

void addQuad(MeshBuilder& mesh, const std::array<glm::vec3, 4>& corners, glm::vec3 color) {
    assert(mesh.vertices.size() + 4 <= kMaxBatchVertices);

    const auto start = static_cast<uint16_t>(mesh.vertices.size());
    const uint32_t packedColor = glm::packUnorm4x8(glm::vec4(color, 1.0f));
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

    const orientation::Basis basis = orientation::fromYawPitchDegrees(yawDegrees, 0.0f);
    std::array<glm::vec3, 8> v{};
    for (size_t i = 0; i < local.size(); ++i) {
        v[i] = center + basis.transform(local[i]);
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

    const orientation::Basis basis = orientation::fromYawPitchDegrees(yawDegrees, pitchDegrees);
    std::array<glm::vec3, 8> v{};
    for (size_t i = 0; i < local.size(); ++i) {
        v[i] = neckPosition + basis.transform(local[i]);
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
    const uint32_t packedColor = glm::packUnorm4x8(glm::vec4(color, 1.0f));
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

bool RenderContext::initialize(int width, int height, const char* title, const std::string& baseDir) {
    baseDir_ = baseDir;
    framebufferWidth_ = width;
    framebufferHeight_ = height;
    windowWidth_ = width;
    windowHeight_ = height;

#if defined(__linux__)
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif
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
    if (api == "opengl") init.type = bgfx::RendererType::OpenGL;
    else if (api == "vulkan") init.type = bgfx::RendererType::Vulkan;
#if defined(_WIN32)
    else if (api == "dx12") init.type = bgfx::RendererType::Direct3D12;
    else init.type = bgfx::RendererType::Direct3D11;
    init.platformData.nwh = glfwGetWin32Window(window_);
#elif defined(__linux__)
    else init.type = bgfx::RendererType::OpenGL;
    init.platformData.ndt = glfwGetX11Display();
    init.platformData.nwh = reinterpret_cast<void*>(static_cast<uintptr_t>(glfwGetX11Window(window_)));
#endif
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

    glfwSetWindowUserPointer(window_, this);
    glfwSetWindowFocusCallback(window_, [](GLFWwindow* window, int focused) {
        auto* context = static_cast<RenderContext*>(glfwGetWindowUserPointer(window));
        if (!focused) {
            context->releaseMouse();
            context->console_.openingCharacter = 0;
        }
    });
    if (!initializeImGui()) {
        shutdown();
        return false;
    }

    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    logging::info("Renderer initialized with GLFW/bgfx ({})", bgfx::getRendererName(bgfx::getRendererType()));
    return true;
}

void RenderContext::shutdown() {
    releaseMouse();
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
}

RenderContext::StartMenuAction RenderContext::renderStartMenu(char* addressBuffer, size_t addressBufferSize, int& port) {
    if (!window_ || !bgfxInitialized_) {
        return StartMenuAction::None;
    }

    releaseMouse();

    bgfx::setViewRect(kMainView, 0, 0, static_cast<uint16_t>(framebufferWidth_), static_cast<uint16_t>(framebufferHeight_));
    bgfx::setViewClear(kMainView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x1b2533ff, 1.0f, 0);
    bgfx::touch(kMainView);

    StartMenuAction action = StartMenuAction::None;
    if (imguiContext_) {
        ImGui::SetCurrentContext(imguiContext_);

        ImGui::SetNextWindowPos(ImVec2(windowWidth_ * 0.5f, windowHeight_ * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_Always);
        constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin("Mineworld", nullptr, kWindowFlags)) {
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
        imguiFrameActive_ = false;
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

    bgfx::setViewRect(kMainView, 0, 0, static_cast<uint16_t>(framebufferWidth_), static_cast<uint16_t>(framebufferHeight_));
    bgfx::setViewClear(kMainView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x1b2533ff, 1.0f, 0);
    bgfx::touch(kMainView);

    ConnectingAction action = ConnectingAction::None;
    if (imguiContext_) {
        ImGui::SetCurrentContext(imguiContext_);
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
        imguiFrameActive_ = false;
        renderImGuiDrawData(ImGui::GetDrawData());
    }

    bgfx::frame();
    return action;
}

void RenderContext::pollEvents() {
    MW_PROFILE_SCOPE("Client.PollEvents");

    glfwPollEvents();
}

void RenderContext::beginFrame() {
    if (!window_ || !imguiContext_) return;
    ImGui::SetCurrentContext(imguiContext_);
    updateDisplayMetrics();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    imguiFrameActive_ = true;
}

void RenderContext::endFrame() {
    if (!imguiFrameActive_) return;
    ImGui::SetCurrentContext(imguiContext_);
    ImGui::EndFrame();
    imguiFrameActive_ = false;
}

void RenderContext::processInput(glm::vec3& rotation, PlayerComponent& player, ControllerInputComponent& input) {
    input.move = glm::vec3(0.0f);
    input.jump = false;
    input.sprint = false;
    if (!window_ || !imguiFrameActive_ || !gameActive_ || !glfwGetWindowAttrib(window_, GLFW_FOCUSED)) return;

    ImGui::SetCurrentContext(imguiContext_);
    const bool escapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (console_.open) {
        if (escapePressed) {
            console_.open = false;
            console_.lastActivityTime = std::chrono::steady_clock::now();
            console_.openingCharacter = 0;
        }
        return;
    }

    if (escapePressed) {
        inGameMenuOpen_ = !inGameMenuOpen_;
        if (inGameMenuOpen_) releaseMouse();
        else captureMouse();
        return;
    }
    if (inGameMenuOpen_) return;

    const auto& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard || io.WantTextInput) return;
    const bool commandPressed = ImGui::IsKeyPressed(ImGuiKey_Slash, false) && !io.KeyShift;
    if (!io.KeyCtrl && !io.KeyAlt && !io.KeySuper && (ImGui::IsKeyPressed(ImGuiKey_T, false) || commandPressed)) {
        openConsole(commandPressed);
        return;
    }

    if (io.KeyAlt) releaseMouse();
    else captureMouse();

    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) profilerMode_ = cycleMode(profilerMode_);
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) cursorMode_ = cycleMode(cursorMode_);
    if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) showChunkBounds_ = !showChunkBounds_;
    if (mouseCaptured_ && ImGui::IsKeyPressed(ImGuiKey_F4, false)) {
        player.mode = player.mode == PlayerMode::Spectator ? PlayerMode::Survival : PlayerMode::Spectator;
        logging::info("Switched player mode to {}", player.mode == PlayerMode::Spectator ? "spectator" : "survival");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false) && player.mode == PlayerMode::Survival) cameraViewMode_ = cycleMode(cameraViewMode_);
    if (!mouseCaptured_) return;

    double mouseX = 0.0;
    double mouseY = 0.0;
    glfwGetCursorPos(window_, &mouseX, &mouseY);
    if (!hasMousePosition_) {
        lastMouseX_ = mouseX;
        lastMouseY_ = mouseY;
        hasMousePosition_ = true;
    }
    constexpr float kMouseSensitivity = 0.12f;
    const float mouseDeltaX = static_cast<float>(mouseX - lastMouseX_);
    const float mouseDeltaY = static_cast<float>(mouseY - lastMouseY_);
    lastMouseX_ = mouseX;
    lastMouseY_ = mouseY;
    rotation.y += mouseDeltaX * kMouseSensitivity;
    rotation.x = std::clamp(rotation.x - mouseDeltaY * kMouseSensitivity, -88.0f, 88.0f);
    cameraYaw_ = rotation.y;
    cameraPitch_ = rotation.x;

    input.sprint = ImGui::IsKeyDown(ImGuiKey_LeftCtrl);
    if (ImGui::IsKeyDown(ImGuiKey_W)) input.move.x += 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_S)) input.move.x -= 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_A)) input.move.z -= 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_D)) input.move.z += 1.0f;
    if (glm::dot(input.move, input.move) > 1.0f) input.move = glm::normalize(input.move);
    if (player.mode == PlayerMode::Spectator) {
        if (ImGui::IsKeyDown(ImGuiKey_Space)) input.move.y += 1.0f;
        if (ImGui::IsKeyDown(ImGuiKey_LeftShift)) input.move.y -= 1.0f;
    } else {
        input.jump = ImGui::IsKeyPressed(ImGuiKey_Space, false);
    }
}

void RenderContext::appendConsoleLine(std::string text, bool success) {
    constexpr size_t kMaxConsoleLines = 200;
    if (console_.lines.size() >= kMaxConsoleLines) console_.lines.pop_front();
    console_.lines.push_back({std::move(text), success});
    console_.lastActivityTime = std::chrono::steady_clock::now();
    console_.scrollToBottom = true;
}

std::optional<std::string> RenderContext::consumeConsoleInput() {
    if (console_.pendingInputs.empty()) {
        return std::nullopt;
    }
    std::string text = std::move(console_.pendingInputs.front());
    console_.pendingInputs.pop_front();
    return text;
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

    constexpr float kEyeHeight = 1.62f;
    constexpr float kCameraDistance = 4.0f;
    constexpr float kThirdPersonTargetHeight = 0.85f;
    const glm::vec3 eyePosition = position + glm::vec3(0.0f, kEyeHeight, 0.0f);
    const glm::vec3 thirdPersonTarget = position + glm::vec3(0.0f, kThirdPersonTargetHeight, 0.0f);

    switch (cameraViewMode_) {
        case CameraViewMode::FirstPerson:
            cameraPosition_ = eyePosition;
            break;
        case CameraViewMode::ThirdPersonFront: {
            cameraYaw_ = yaw + 180.0f;
            cameraPitch_ = -pitch;
            cameraPosition_ = thirdPersonTarget - cameraBasis().forward * kCameraDistance;
            break;
        }
        case CameraViewMode::ThirdPersonBack:
            cameraPitch_ = pitch;
            cameraPosition_ = thirdPersonTarget - cameraBasis().forward * kCameraDistance;
            break;
        default:
            break;
    }
}

void RenderContext::render(const ActorWorld& actorWorld, ClientChunkManager& chunkManager, ChunkCuller& chunkCuller) {
    if (!window_ || !bgfxInitialized_) {
        return;
    }

    bgfx::setViewMode(kMainView, bgfx::ViewMode::DepthAscending);
    bgfx::setViewRect(kMainView, 0, 0, static_cast<uint16_t>(framebufferWidth_), static_cast<uint16_t>(framebufferHeight_));
    bgfx::setViewClear(kMainView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x87bdf2ff, 1.0f, 0);

    const glm::vec3 target = cameraPosition_ + cameraBasis().forward;
    const bx::Vec3 eye(cameraPosition_.x, cameraPosition_.y, cameraPosition_.z);
    const bx::Vec3 at(target.x, target.y, target.z);
    float view[16];
    float renderProjection[16];
    const float aspectRatio = static_cast<float>(framebufferWidth_) / framebufferHeight_;
    bx::mtxLookAt(view, eye, at, bx::Vec3(0.0f, 1.0f, 0.0f), bx::Handedness::Right);
    bx::mtxProj(
        renderProjection,
        kVerticalFieldOfView,
        aspectRatio,
        kNearPlane,
        kFarPlane,
        bgfx::getCaps()->homogeneousDepth,
        bx::Handedness::Right);
    bgfx::setViewTransform(kMainView, view, renderProjection);
    bgfx::touch(kMainView);

    float cullingProjection[16];
    bx::mtxProj(
        cullingProjection,
        kVerticalFieldOfView,
        aspectRatio,
        kNearPlane,
        kFarPlane,
        true,
        bx::Handedness::Right);
    float cullingViewProjection[16];
    bx::mtxMul(cullingViewProjection, view, cullingProjection);
    renderWorld(actorWorld, chunkManager, chunkCuller, Frustum::fromViewProjection(cullingViewProjection));

    recordBgfxStats(bgfx::getStats());

    if (imguiContext_) {
        ImGui::SetCurrentContext(imguiContext_);

        renderEntityNames(actorWorld, cullingViewProjection);
        if (profilerMode_ != ProfilerMode::Hidden) {
            renderProfilerOverlay();
        }
        if (cursorMode_ != CursorMode::Hidden) {
            renderCursorOverlay();
        }
        if (inGameMenuOpen_) {
            renderInGameMenu();
        }
        renderConsole();

        ImGui::Render();
        imguiFrameActive_ = false;
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
    if (!window_ || mouseCaptured_ || console_.open || inGameMenuOpen_ || !glfwGetWindowAttrib(window_, GLFW_FOCUSED)) {
        return;
    }
    mouseCaptured_ = true;
    ImGui::SetCurrentContext(imguiContext_);
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    hasMousePosition_ = false;
}

void RenderContext::releaseMouse() {
    if (!window_ || !mouseCaptured_) {
        return;
    }
    mouseCaptured_ = false;
    ImGui::SetCurrentContext(imguiContext_);
    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    hasMousePosition_ = false;
}

void RenderContext::resetInGameMenu() {
    inGameMenuOpen_ = false;
    pendingInGameMenuAction_ = InGameMenuAction::None;
    gameActive_ = false;
    console_ = {};
}

RenderContext::InGameMenuAction RenderContext::consumeInGameMenuAction() {
    const InGameMenuAction action = pendingInGameMenuAction_;
    pendingInGameMenuAction_ = InGameMenuAction::None;
    return action;
}

bool RenderContext::loadProgram(const char* vertexName, const char* fragmentName, uint16_t& program) const {
    const char* rendererDir = shaderDirectoryForRenderer(bgfx::getRendererType());
    const std::filesystem::path shaderDir = std::filesystem::path(baseDir_) / "shaders" / rendererDir;
    const std::vector<uint8_t> vertexShaderData = readBinaryFile(shaderDir / vertexName);
    const std::vector<uint8_t> fragmentShaderData = readBinaryFile(shaderDir / fragmentName);
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
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().Colors[ImGuiCol_Text] = ImVec4(1, 1, 1, 1);
    if (!ImGui_ImplGlfw_InitForOther(window_, true)) {
        logging::error("Failed to initialize ImGui GLFW backend");
        shutdownImGui();
        return false;
    }
    const AppConfig& config = AppConfig::instance();
    std::filesystem::path fontPath = config.fontPath;
    if (fontPath.is_relative()) fontPath = std::filesystem::path(baseDir_) / fontPath;
    fontPath = fontPath.lexically_normal();

    std::error_code fontPathError;
    if (std::filesystem::is_regular_file(fontPath, fontPathError)) {
        fontData_ = readBinaryFile(fontPath);
        if (!fontData_.empty()) {
            ImFontConfig fontConfig;
            fontConfig.FontDataOwnedByAtlas = false;
            if (!io.Fonts->AddFontFromMemoryTTF(fontData_.data(), static_cast<int>(fontData_.size()), config.fontSize, &fontConfig)) {
                logging::warn("Failed to load font from {}, using the default font", fontPath.string());
                fontData_.clear();
            }
        } else {
            logging::warn("Failed to read font from {}, using the default font", fontPath.string());
        }
    } else {
        logging::warn("Font not found at {}, using the default font", fontPath.string());
    }
    if (!io.Fonts->Fonts.empty()) {
        const std::filesystem::path fallbackFontPath = (std::filesystem::path(baseDir_) / "../fonts/DroidSansFallbackFull.ttf").lexically_normal();
        fallbackFontData_ = readBinaryFile(fallbackFontPath);
        if (!fallbackFontData_.empty()) {
            ImFontConfig fallbackFontConfig;
            fallbackFontConfig.FontDataOwnedByAtlas = false;
            fallbackFontConfig.MergeMode = true;
            if (!io.Fonts->AddFontFromMemoryTTF(
                    fallbackFontData_.data(),
                    static_cast<int>(fallbackFontData_.size()),
                    config.fontSize,
                    &fallbackFontConfig,
                    io.Fonts->GetGlyphRangesChineseFull())) {
                logging::warn("Failed to load CJK fallback font from {}", fallbackFontPath.string());
                fallbackFontData_.clear();
            }
        } else {
            logging::warn("CJK fallback font not found at {}", fallbackFontPath.string());
        }
    } else {
        io.Fonts->AddFontDefault();
    }

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
        endFrame();
        if (ImGui::GetIO().BackendPlatformUserData) ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext(imguiContext_);
        imguiContext_ = nullptr;
    }
    fontData_.clear();
    fallbackFontData_.clear();
}

void RenderContext::renderWorld(const ActorWorld& actorWorld, const ClientChunkManager& chunkManager, ChunkCuller& chunkCuller, const Frustum& frustum) {
    MW_PROFILE_SCOPE("Render.World");

    const ChunkRenderView renderData = chunkManager.renderData();
    const auto chunks = renderData.chunks;
    {
        MW_PROFILE_SCOPE("Render.World.ChunkCulling");
        chunkCuller.cull(renderData, frustum, cameraPosition_);
    }

    const auto visibleIndices = chunkCuller.visibleChunkIndices();
    MW_PROFILE_GAUGE("Render.LoadedChunks", static_cast<double>(chunks.size()));
    MW_PROFILE_GAUGE("Render.MeshCacheSize", static_cast<double>(chunkManager.meshCount()));
    MW_PROFILE_GAUGE("Render.ChunksVisible", static_cast<double>(visibleIndices.size()));
    MW_PROFILE_GAUGE("Render.ChunksCulled", static_cast<double>(chunks.size()) - visibleIndices.size());

    {
        MW_PROFILE_SCOPE("Render.World.SubmitChunkMesh");

        const bgfx::IndexBufferHandle quadIndexBuffer{chunkManager.quadIndexBuffer()};
        if (bgfx::isValid(quadIndexBuffer)) {
            int64_t submittedChunks = 0;
            int64_t submittedVertices = 0;
            for (uint32_t index : visibleIndices) {
                const DrawableChunk& visible = chunks[index];
                const ChunkMeshBinding& binding = visible.binding;
                if (!binding.isValid()) {
                    continue;
                }
                const glm::ivec3 chunkPos = visible.chunkPos;
                const glm::vec3 center = (glm::vec3(chunkPos) + glm::vec3(0.5f)) * static_cast<float>(ChunkLayout::kSize);
                const glm::vec3 offset = center - cameraPosition_;

                float model[16];
                bx::mtxTranslate(model, static_cast<float>(chunkPos.x * ChunkLayout::kSize), static_cast<float>(chunkPos.y * ChunkLayout::kSize), static_cast<float>(chunkPos.z * ChunkLayout::kSize));
                bgfx::setTransform(model);
                bgfx::setVertexBuffer(0, bgfx::DynamicVertexBufferHandle{binding.vertexBuffer}, binding.vertexOffset, binding.vertexCount);
                bgfx::setIndexBuffer(quadIndexBuffer, 0, ChunkMeshPool::indexCountForVertices(binding.vertexCount));
                bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CW);
                bgfx::submit(kMainView, bgfx::ProgramHandle{chunkShader_.program}, depthSortKey(glm::dot(offset, offset)));
                ++submittedChunks;
                submittedVertices += binding.vertexCount;
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
            for (const DrawableChunk& chunk : chunks) {
                if (lineBatch.vertices.size() + kLineBoxVertexCount > kMaxBatchVertices) {
                    submitLineBatch(lineBatch, unlitShader_.program, kDepthLast);
                    lineBatch.vertices.clear();
                    lineBatch.indices.clear();
                }

                const glm::vec3 min = glm::vec3(chunk.chunkPos) * static_cast<float>(ChunkLayout::kSize);
                const glm::vec3 max = min + glm::vec3(static_cast<float>(ChunkLayout::kSize));
                addLineBox(lineBatch, min, max, boundColor);
            }
            submitLineBatch(lineBatch, unlitShader_.program, kDepthLast);
        }
    }
}

void RenderContext::renderEntityNames(const ActorWorld& actorWorld, const float* viewProjection) {
    MW_PROFILE_SCOPE("Render.EntityNames");
    constexpr float kMaxDistance = 64.0f;
    const auto& registry = actorWorld.registry();
    const auto view = registry.view<NameComponent, TransformComponent, MeshComponent>();
    auto* drawList = ImGui::GetBackgroundDrawList();
    for (const auto entity : view) {
        const auto& name = view.get<NameComponent>(entity).name;
        if (name.empty() || !view.get<MeshComponent>(entity).isVisible || shouldHideLocalPlayerModel(actorWorld, entity)) {
            continue;
        }
        const auto& transform = view.get<TransformComponent>(entity);
        float height = 1.8f;
        if (const auto* collider = registry.try_get<BoxColliderComponent>(entity)) {
            height = collider->offset.y + collider->size.y * 0.5f;
        }
        const glm::vec3 anchor = transform.position + glm::vec3(0.0f, height + 0.25f, 0.0f);
        const glm::vec3 offset = anchor - cameraPosition_;
        if (glm::dot(offset, offset) > kMaxDistance * kMaxDistance) {
            continue;
        }
        float clip[4];
        for (int row = 0; row < 4; ++row) {
            clip[row] = viewProjection[row] * anchor.x + viewProjection[4 + row] * anchor.y + viewProjection[8 + row] * anchor.z + viewProjection[12 + row];
        }
        if (clip[3] <= 0.0f || clip[2] < -clip[3] || clip[2] > clip[3] || std::abs(clip[0]) > clip[3] || std::abs(clip[1]) > clip[3]) {
            continue;
        }
        const ImVec2 size = ImGui::CalcTextSize(name.c_str());
        const ImVec2 origin((clip[0] / clip[3] * 0.5f + 0.5f) * windowWidth_ - size.x * 0.5f,
                            (0.5f - clip[1] / clip[3] * 0.5f) * windowHeight_ - size.y);
        drawList->AddRectFilled(ImVec2(origin.x - 4.0f, origin.y - 2.0f),
                                ImVec2(origin.x + size.x + 4.0f, origin.y + size.y + 2.0f),
                                IM_COL32(0, 0, 0, 150), 3.0f);
        drawList->AddText(origin, IM_COL32(255, 255, 255, 255), name.c_str());
    }
}

void RenderContext::renderProfilerOverlay() {
    MW_PROFILE_SCOPE("Render.Profiler");

    const profiling::Snapshot snapshot = profiling::Profiler::instance().snapshot();

    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.82f);
    constexpr ImGuiWindowFlags kWindowFlags =
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

    if (ImGui::Begin("ProfilerOverlay", nullptr, kWindowFlags)) {
        char buffer[128];
        const glm::ivec3 chunkCoord = ChunkLayout::worldToChunk(cameraPosition_);
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
    constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Game Menu", nullptr, kWindowFlags)) {
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
        const orientation::Basis basis = cameraBasis();
        const glm::vec3 cameraRight = basis.right;
        const glm::vec3 cameraUp = basis.up;

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

void RenderContext::openConsole(bool command) {
    console_.open = true;
    console_.focusInput = true;
    console_.positionCursor = true;
    console_.openingCharacter = command ? '/' : 't';
    if (command) console_.history.edit("/");
    const auto& text = console_.history.current();
    std::memcpy(console_.input.data(), text.c_str(), text.size() + 1);
    console_.scrollToBottom = true;
    releaseMouse();
}

void RenderContext::renderConsole() {
    MW_PROFILE_SCOPE("Render.Console");
    float alpha = 1.0f;
    if (!console_.open) {
        if (console_.lines.empty()) return;
        const auto age = std::chrono::steady_clock::now() - console_.lastActivityTime;
        if (age >= kConsoleMessageDisplayDuration) return;
        const float remaining = std::chrono::duration<float>(kConsoleMessageDisplayDuration - age).count();
        alpha = std::clamp(remaining / std::chrono::duration<float>(kConsoleMessageFadeDuration).count(), 0.0f, 1.0f);
        alpha *= alpha;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * alpha);
    ImGui::SetNextWindowPos(ImVec2(10.0f, windowHeight_ - 10.0f), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowSize(ImVec2(std::max(100.0f, std::min(640.0f, windowWidth_ - 20.0f)), std::min(300.0f, windowHeight_ * 0.45f)), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha((console_.open ? 0.75f : 0.3f) * ImGui::GetStyle().Alpha);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
    if (!console_.open) flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("GameConsole", nullptr, flags)) {
        const float inputHeight = console_.open ? ImGui::GetFrameHeightWithSpacing() : 0.0f;
        if (ImGui::BeginChild("Messages", ImVec2(0, -inputHeight), ImGuiChildFlags_None)) {
            for (const auto& line : console_.lines) {
                ImGui::PushStyleColor(ImGuiCol_Text, line.success ? ImVec4(1, 1, 1, 1) : ImVec4(1, 0.25f, 0.25f, 1));
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(line.text.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            if (console_.scrollToBottom) {
                ImGui::SetScrollHereY(1.0f);
                console_.scrollToBottom = false;
            }
        }
        ImGui::EndChild();
        if (console_.open) {
            auto& characters = ImGui::GetIO().InputQueueCharacters;
            if (console_.openingCharacter != 0 && !characters.empty()) {
                const auto opening = std::find_if(characters.begin(), characters.end(), [&](ImWchar character) {
                    return character == console_.openingCharacter || (console_.openingCharacter == 't' && character == 'T');
                });
                if (opening != characters.end()) characters.erase(characters.begin(), opening + 1);
                console_.openingCharacter = 0;
            }
            if (console_.focusInput) {
                if (!characters.empty()) {
                    std::string text = console_.input.data();
                    for (const auto character : characters) {
                        if (character >= 0x20 && character != 0x7f) appendUtf8Codepoint(text, character, kMaxInputTextBytes);
                    }
                    console_.history.edit(text);
                    std::memcpy(console_.input.data(), text.c_str(), text.size() + 1);
                    characters.resize(0);
                }
                ImGui::SetKeyboardFocusHere();
                console_.focusInput = false;
            }
            ImGui::SetNextItemWidth(-1.0f);
            const auto callback = [](ImGuiInputTextCallbackData* data) {
                auto* console = static_cast<ConsoleState*>(data->UserData);
                if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
                    if (data->EventKey == ImGuiKey_UpArrow) console->history.previous();
                    else if (data->EventKey == ImGuiKey_DownArrow) console->history.next();
                    const auto& text = console->history.current();
                    data->DeleteChars(0, data->BufTextLen);
                    data->InsertChars(0, text.c_str());
                } else if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
                    console->history.edit(std::string(data->Buf, data->BufTextLen));
                }
                if (console->positionCursor) {
                    data->CursorPos = data->SelectionStart = data->SelectionEnd = data->BufTextLen;
                    console->positionCursor = false;
                }
                return 0;
            };
            constexpr auto kInputFlags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackAlways;
            if (ImGui::InputText("##ConsoleInput", console_.input.data(), console_.input.size(), kInputFlags, callback, &console_)) {
                if (!trimText(console_.input.data()).empty()) console_.pendingInputs.emplace_back(console_.input.data());
                console_.history.submit();
                console_.input[0] = '\0';
                console_.focusInput = true;
                console_.positionCursor = true;
                console_.openingCharacter = 0;
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

orientation::Basis RenderContext::cameraBasis() const {
    return orientation::fromYawPitchDegrees(cameraYaw_, cameraPitch_);
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
