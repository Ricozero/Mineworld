#include "render_context.h"

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/math.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <vector>

#include "chunk.h"
#include "client_world.h"
#include "entity.h"
#include "log.h"
#include "profiler.h"

namespace {

constexpr bgfx::ViewId kMainView = 0;
constexpr bgfx::ViewId kImGuiView = 1;
constexpr uint32_t kResetFlags = BGFX_RESET_VSYNC;

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
            return glm::vec3(0.43f, 0.28f, 0.16f);
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

void addQuad(MeshBuilder& mesh, const std::array<glm::vec3, 4>& corners, glm::vec3 color) {
    if (mesh.vertices.size() > UINT16_MAX - 4) {
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

void addBox(MeshBuilder& mesh, glm::vec3 min, glm::vec3 max, glm::vec3 color) {
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
    addQuad(mesh, {{v[0], v[3], v[2], v[1]}}, color * 0.65f);
    addQuad(mesh, {{v[4], v[5], v[6], v[7]}}, color * 0.90f);
    addQuad(mesh, {{v[0], v[1], v[5], v[4]}}, color * 0.55f);
    addQuad(mesh, {{v[3], v[7], v[6], v[2]}}, color);
    addQuad(mesh, {{v[1], v[2], v[6], v[5]}}, color * 0.82f);
    addQuad(mesh, {{v[0], v[4], v[7], v[3]}}, color * 0.72f);
}

void addLineBox(MeshBuilder& mesh, glm::vec3 min, glm::vec3 max, glm::vec3 color) {
    if (mesh.vertices.size() > UINT16_MAX - 8) {
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
    MW_PROFILE_SCOPE("Render.SubmitLineBatch");

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
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_PT_LINES);
        bgfx::submit(kMainView, bgfx::ProgramHandle{programIndex});
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

    profiling::Profiler& profiler = profiling::Profiler::instance();
    profiler.setGauge("BGFX.CPUFrameMs", timestampMs(stats->cpuTimeFrame, stats->cpuTimerFreq));
    profiler.setGauge("BGFX.CPUSubmitMs", timestampRangeMs(stats->cpuTimeBegin, stats->cpuTimeEnd, stats->cpuTimerFreq));
    profiler.setGauge("BGFX.GPUFrameMs", timestampRangeMs(stats->gpuTimeBegin, stats->gpuTimeEnd, stats->gpuTimerFreq));
    profiler.setGauge("BGFX.WaitRenderMs", timestampMs(stats->waitRender, stats->cpuTimerFreq));
    profiler.setGauge("BGFX.WaitSubmitMs", timestampMs(stats->waitSubmit, stats->cpuTimerFreq));
    profiler.setGauge("BGFX.DrawCalls", static_cast<double>(stats->numDraw));
    profiler.setGauge("BGFX.TransientVB", static_cast<double>(stats->transientVbUsed));
    profiler.setGauge("BGFX.TransientIB", static_cast<double>(stats->transientIbUsed));
}

void submitMeshBatch(const MeshBuilder& mesh, unsigned short programIndex) {
    MW_PROFILE_SCOPE("Render.SubmitMeshBatch");

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
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
        bgfx::submit(kMainView, bgfx::ProgramHandle{programIndex});
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
    init.type = bgfx::RendererType::Direct3D11;
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

void RenderContext::pollEvents() {
    MW_PROFILE_SCOPE("Client.PollEvents");

    glfwPollEvents();
}

void RenderContext::processInput(float deltaTime, glm::vec3& position, glm::vec3& rotation, PlayerComponent& player) {
    MW_PROFILE_SCOPE("Client.ProcessInput");

    if (!window_) {
        return;
    }

    deltaTime = std::clamp(deltaTime, 0.0f, 0.05f);

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
        if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window_, GLFW_TRUE);
        }

        const bool f1Down = glfwGetKey(window_, GLFW_KEY_F1) == GLFW_PRESS;
        if (f1Down && !prevF1Down_) {
            showProfiler_ = !showProfiler_;
        }
        prevF1Down_ = f1Down;

        const bool f2Down = glfwGetKey(window_, GLFW_KEY_F2) == GLFW_PRESS;
        if (f2Down && !prevF2Down_) {
            cursorMode_ = (cursorMode_ == CursorMode::None)
                              ? CursorMode::Cross
                          : (cursorMode_ == CursorMode::Cross)
                              ? CursorMode::XYZ
                              : CursorMode::None;
        }
        prevF2Down_ = f2Down;

        const bool f3Down = glfwGetKey(window_, GLFW_KEY_F3) == GLFW_PRESS;
        if (f3Down && !prevF3Down_) {
            showChunkBounds_ = !showChunkBounds_;
        }
        prevF3Down_ = f3Down;

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

    const bool spectatorMode = player.mode == PlayerMode::Spectator;
    const float baseSpeed = spectatorMode ? player.spectatorMoveSpeed : player.survivalMoveSpeed;
    const float sprintMultiplier = spectatorMode ? 5.0f : 1.6f;
    const bool sprinting = glfwGetKey(window_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
    const float moveSpeed = sprinting ? baseSpeed * sprintMultiplier : baseSpeed;
    const float moveStep = moveSpeed * deltaTime;
    glm::vec3 moveForward = forward();
    glm::vec3 moveRight = right();
    if (!spectatorMode) {
        moveForward.y = 0.0f;
        moveRight.y = 0.0f;
        if (glm::dot(moveForward, moveForward) > 0.0001f) {
            moveForward = glm::normalize(moveForward);
        }
        if (glm::dot(moveRight, moveRight) > 0.0001f) {
            moveRight = glm::normalize(moveRight);
        }
    }

    if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) {
        position += moveForward * moveStep;
    }
    if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) {
        position -= moveForward * moveStep;
    }
    if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) {
        position -= moveRight * moveStep;
    }
    if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) {
        position += moveRight * moveStep;
    }
    if (spectatorMode && glfwGetKey(window_, GLFW_KEY_SPACE) == GLFW_PRESS) {
        position.y += moveStep;
    }
    if (spectatorMode && glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
        position.y -= moveStep;
    }
    if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }

    // Update position in camera state for rendering
    cameraPosition_ = position;

    const bool f1Down = glfwGetKey(window_, GLFW_KEY_F1) == GLFW_PRESS;
    if (f1Down && !prevF1Down_) {
        showProfiler_ = !showProfiler_;
    }
    prevF1Down_ = f1Down;

    const bool f2Down = glfwGetKey(window_, GLFW_KEY_F2) == GLFW_PRESS;
    if (f2Down && !prevF2Down_) {
        cursorMode_ = (cursorMode_ == CursorMode::None)
                          ? CursorMode::Cross
                      : (cursorMode_ == CursorMode::Cross)
                          ? CursorMode::XYZ
                          : CursorMode::None;
    }
    prevF2Down_ = f2Down;

    const bool f3Down = glfwGetKey(window_, GLFW_KEY_F3) == GLFW_PRESS;
    if (f3Down && !prevF3Down_) {
        showChunkBounds_ = !showChunkBounds_;
    }
    prevF3Down_ = f3Down;
}

void RenderContext::setCamera(const glm::vec3& position, float yaw, float pitch) {
    cameraPosition_ = position;
    cameraYaw_ = yaw;
    cameraPitch_ = pitch;
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

    if (showProfiler_) {
        renderProfilerOverlay(deltaTime);
    }

    if (cursorMode_ != CursorMode::None) {
        renderCursorOverlay(deltaTime);
    }

    bgfx::frame();
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

    programIndex_ = program.idx;
    return true;
}

void RenderContext::destroyShaders() {
    bgfx::ProgramHandle program{programIndex_};
    if (bgfx::isValid(program)) {
        bgfx::destroy(program);
        programIndex_ = bgfx::kInvalidHandle;
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
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
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
    imguiFontTextureIndex_ = fontTexture.idx;

    bgfx::UniformHandle textureUniform = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
    if (!bgfx::isValid(textureUniform)) {
        logging::error("Failed to create ImGui texture uniform");
        shutdownImGui();
        return false;
    }
    imguiTextureUniformIndex_ = textureUniform.idx;

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
    imguiProgramIndex_ = program.idx;
    return true;
}

void RenderContext::shutdownImGui() {
    bgfx::ProgramHandle program{imguiProgramIndex_};
    if (bgfx::isValid(program)) {
        bgfx::destroy(program);
        imguiProgramIndex_ = bgfx::kInvalidHandle;
    }

    bgfx::UniformHandle textureUniform{imguiTextureUniformIndex_};
    if (bgfx::isValid(textureUniform)) {
        bgfx::destroy(textureUniform);
        imguiTextureUniformIndex_ = bgfx::kInvalidHandle;
    }

    bgfx::TextureHandle fontTexture{imguiFontTextureIndex_};
    if (bgfx::isValid(fontTexture)) {
        bgfx::destroy(fontTexture);
        imguiFontTextureIndex_ = bgfx::kInvalidHandle;
    }

    if (imguiContext_) {
        ImGui::SetCurrentContext(imguiContext_);
        ImGui::DestroyContext(imguiContext_);
        imguiContext_ = nullptr;
    }
}

void RenderContext::renderWorld(const ClientWorld& world) {
    MW_PROFILE_SCOPE("Render.World");

    const VoxelWorld& voxelWorld = world.getVoxelWorld();
    const auto loadedChunks = voxelWorld.getLoadedChunks();
    MW_PROFILE_GAUGE("World.LoadedChunks", static_cast<double>(loadedChunks.size()));

    // Remove cached meshes for unloaded chunks
    std::unordered_set<glm::ivec3> loadedSet(loadedChunks.begin(), loadedChunks.end());
    std::vector<glm::ivec3> staleChunks;
    staleChunks.reserve(chunkMeshCache_.size());
    for (const auto& [chunkPos, _] : chunkMeshCache_) {
        if (loadedSet.find(chunkPos) == loadedSet.end()) {
            staleChunks.push_back(chunkPos);
        }
    }
    for (const glm::ivec3& chunkPos : staleChunks) {
        chunkBlockCounts_.erase(chunkPos);
        chunkMeshCache_.erase(chunkPos);
    }

    // Build/update cached meshes per chunk (only when dirty)
    std::unordered_map<glm::ivec3, size_t> currentCounts;
    currentCounts.reserve(loadedChunks.size());
    for (const glm::ivec3& chunkPos : loadedChunks) {
        const Chunk& chunk = voxelWorld.getChunk(chunkPos);
        currentCounts[chunkPos] = chunk.getBlockCount();
    }

    for (const glm::ivec3& chunkPos : loadedChunks) {
        const size_t blockCount = currentCounts[chunkPos];

        auto countIt = chunkBlockCounts_.find(chunkPos);
        bool needsRebuild = (countIt == chunkBlockCounts_.end()) ||
                            (countIt->second != blockCount) ||
                            (chunkMeshCache_.find(chunkPos) == chunkMeshCache_.end());

        if (!needsRebuild) {
            static const std::array<glm::ivec3, 6> kNeighborOffsets = {{glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 1, 0),
                                                                        glm::ivec3(0, -1, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1)}};
            for (const glm::ivec3& off : kNeighborOffsets) {
                const glm::ivec3 neighbor = chunkPos + off;
                auto prevNeighborIt = chunkBlockCounts_.find(neighbor);
                auto currNeighborIt = currentCounts.find(neighbor);
                if (prevNeighborIt != chunkBlockCounts_.end() && currNeighborIt != currentCounts.end()) {
                    if (prevNeighborIt->second != currNeighborIt->second) {
                        needsRebuild = true;
                        break;
                    }
                }
            }
        }

        if (needsRebuild) {
            CachedChunkMesh cachedMesh;
            buildChunkMesh(world, chunkPos, cachedMesh);
            MW_PROFILE_COUNTER("Chunk.MeshRebuilds", 1);
            MW_PROFILE_COUNTER("Chunk.MeshBuildVertices", static_cast<int64_t>(cachedMesh.vertexCount));
            MW_PROFILE_COUNTER("Chunk.MeshBuildIndices", static_cast<int64_t>(cachedMesh.indices.size()));
            chunkBlockCounts_[chunkPos] = blockCount;
            chunkMeshCache_[chunkPos] = std::move(cachedMesh);
        }
    }
    MW_PROFILE_GAUGE("Chunk.MeshCacheSize", static_cast<double>(chunkMeshCache_.size()));

    // Submit chunk meshes in batches
    MeshBuilder currentBatch;
    currentBatch.vertices.reserve(8192);
    currentBatch.indices.reserve(12288);

    for (const glm::ivec3& chunkPos : loadedChunks) {
        const auto cacheIt = chunkMeshCache_.find(chunkPos);
        if (cacheIt == chunkMeshCache_.end()) {
            continue;
        }
        const auto& cached = cacheIt->second;
        if (cached.vertexCount == 0 || cached.indices.empty()) {
            continue;
        }

        if (currentBatch.vertices.size() + cached.vertexCount > UINT16_MAX) {
            submitMeshBatch(currentBatch, programIndex_);
            currentBatch.vertices.clear();
            currentBatch.indices.clear();
        }

        const auto baseVertex = static_cast<uint16_t>(currentBatch.vertices.size());
        const size_t floatsPerVertex = 4;
        for (size_t i = 0; i < cached.vertexCount; ++i) {
            float x = cached.vertexData[i * floatsPerVertex + 0];
            float y = cached.vertexData[i * floatsPerVertex + 1];
            float z = cached.vertexData[i * floatsPerVertex + 2];
            uint32_t abgr;
            std::memcpy(&abgr, &cached.vertexData[i * floatsPerVertex + 3], sizeof(uint32_t));
            currentBatch.vertices.push_back(PosColorVertex{x, y, z, abgr});
        }
        for (uint16_t idx : cached.indices) {
            currentBatch.indices.push_back(baseVertex + idx);
        }
    }

    // Render entities based on MeshComponent
    const auto& registry = world.getActorWorld().registry();
    auto viewEntities = registry.view<TransformComponent, MeshComponent>();
    MW_PROFILE_GAUGE("World.VisibleEntities", static_cast<double>(viewEntities.size_hint()));
    for (auto entity : viewEntities) {
        const auto& meshComp = registry.get<MeshComponent>(entity);
        if (!meshComp.isVisible) {
            continue;
        }

        if (currentBatch.vertices.size() + 24 > UINT16_MAX) {
            submitMeshBatch(currentBatch, programIndex_);
            currentBatch.vertices.clear();
            currentBatch.indices.clear();
        }
        const auto& transform = registry.get<TransformComponent>(entity);
        const glm::vec3 color(meshComp.color.r, meshComp.color.g, meshComp.color.b);
        const glm::vec3 min = transform.position + glm::vec3(-0.35f, 0.01f, -0.35f);
        const glm::vec3 max = transform.position + glm::vec3(0.35f, 1.81f, 0.35f);
        addBox(currentBatch, min, max, color);
    }

    // Flush remaining batch
    submitMeshBatch(currentBatch, programIndex_);

    if (showChunkBounds_) {
        MeshBuilder lineBatch;
        const glm::vec3 boundColor(1.0f, 0.92f, 0.25f);
        for (const glm::ivec3& chunkPos : loadedChunks) {
            const glm::vec3 min = glm::vec3(chunkPos) * static_cast<float>(Chunk::SIZE);
            const glm::vec3 max = min + glm::vec3(static_cast<float>(Chunk::SIZE));
            addLineBox(lineBatch, min, max, boundColor);
        }
        submitLineBatch(lineBatch, programIndex_);
    }
}

void RenderContext::renderProfilerOverlay(float deltaTime) {
    MW_PROFILE_SCOPE("Overlay.Profiler");

    if (!imguiContext_) {
        return;
    }

    ImGui::SetCurrentContext(imguiContext_);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(windowWidth_), static_cast<float>(windowHeight_));
    io.DisplayFramebufferScale = ImVec2(framebufferScaleX_, framebufferScaleY_);
    io.DeltaTime = deltaTime > 0.0f ? deltaTime : 1.0f / 60.0f;

    ImGui::NewFrame();
    const profiling::Snapshot snapshot = profiling::Profiler::instance().snapshot();
    const bgfx::Stats* stats = bgfx::getStats();
    const double bgfxCpuFrameMs = stats ? timestampMs(stats->cpuTimeFrame, stats->cpuTimerFreq) : 0.0;
    const double bgfxCpuSubmitMs = stats ? timestampRangeMs(stats->cpuTimeBegin, stats->cpuTimeEnd, stats->cpuTimerFreq) : 0.0;
    const double bgfxGpuFrameMs = stats ? timestampRangeMs(stats->gpuTimeBegin, stats->gpuTimeEnd, stats->gpuTimerFreq) : 0.0;
    const double waitRenderMs = stats ? timestampMs(stats->waitRender, stats->cpuTimerFreq) : 0.0;
    const double waitSubmitMs = stats ? timestampMs(stats->waitSubmit, stats->cpuTimerFreq) : 0.0;

    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.82f);
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;

    constexpr float kColName = 220.0f;
    constexpr float kColValue = 70.0f;

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
            std::snprintf(buffer, sizeof(buffer), "%.1f, %.1f, %.1f",
                          cameraPosition_.x, cameraPosition_.y, cameraPosition_.z);
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
            ImGui::TextUnformatted(bgfx::getRendererName(bgfx::getRendererType()));
            ImGui::EndTable();
        }

        if (ImGui::BeginTable("BgfxStats", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("CPU frame", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("CPU submit", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("GPU frame", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Draws", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Wait", ImGuiTableColumnFlags_WidthFixed, 160.0f);
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            rightAlignedText("%.1f ms", bgfxCpuFrameMs);
            ImGui::TableNextColumn();
            rightAlignedText("%.1f ms", bgfxCpuSubmitMs);
            ImGui::TableNextColumn();
            rightAlignedText("%.1f ms", bgfxGpuFrameMs);
            ImGui::TableNextColumn();
            rightAlignedText("%u", stats ? stats->numDraw : 0);
            ImGui::TableNextColumn();
            rightAlignedText("R %.1f / S %.1f", waitRenderMs, waitSubmitMs);
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Top CPU Scopes");
        if (ImGui::BeginTable("ProfilerScopes", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Scope", 0, kColName);
            ImGui::TableSetupColumn("Frame", 0, kColValue);
            ImGui::TableSetupColumn("Avg", 0, kColValue);
            ImGui::TableSetupColumn("Max", 0, kColValue);
            ImGui::TableSetupColumn("Calls", 0, kColValue);
            ImGui::TableHeadersRow();

            int rows = 0;
            for (const profiling::ScopeEntry& entry : snapshot.scopes) {
                if (entry.name == "Frame.Total" || entry.lastFrameMs <= 0.0) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.name.c_str());
                ImGui::TableNextColumn();
                rightAlignedText("%.1f", entry.lastFrameMs);
                ImGui::TableNextColumn();
                rightAlignedText("%.1f", entry.averageFrameMs);
                ImGui::TableNextColumn();
                rightAlignedText("%.1f", entry.maxMs);
                ImGui::TableNextColumn();
                rightAlignedText("%llu", static_cast<unsigned long long>(entry.lastFrameCalls));
                if (++rows >= 8) {
                    break;
                }
            }
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Counters");
        if (ImGui::BeginTable("ProfilerCounters", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Counter", 0, kColName);
            ImGui::TableSetupColumn("Frame", 0, kColValue);
            ImGui::TableSetupColumn("Avg", 0, kColValue);
            ImGui::TableSetupColumn("Total", 0, kColValue);
            ImGui::TableHeadersRow();

            int rows = 0;
            for (const profiling::CounterEntry& entry : snapshot.counters) {
                if (entry.lastFrameValue == 0 && entry.totalValue == 0) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.name.c_str());
                ImGui::TableNextColumn();
                rightAlignedText("%lld", static_cast<long long>(entry.lastFrameValue));
                ImGui::TableNextColumn();
                rightAlignedText("%.1f", entry.averageFrameValue);
                ImGui::TableNextColumn();
                rightAlignedText("%lld", static_cast<long long>(entry.totalValue));
                if (++rows >= 8) {
                    break;
                }
            }
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Gauges");
        if (ImGui::BeginTable("ProfilerGauges", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Gauge", 0, kColName);
            ImGui::TableSetupColumn("Now", 0, kColValue);
            ImGui::TableSetupColumn("Avg", 0, kColValue);
            ImGui::TableSetupColumn("Peak", 0, kColValue);
            ImGui::TableHeadersRow();

            for (const profiling::GaugeEntry& entry : snapshot.gauges) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.name.c_str());
                ImGui::TableNextColumn();
                rightAlignedText("%.1f", entry.value);
                ImGui::TableNextColumn();
                rightAlignedText("%.1f", entry.averageValue);
                ImGui::TableNextColumn();
                rightAlignedText("%.1f", entry.peakValue);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();

    ImGui::Render();
    renderImGuiDrawData(ImGui::GetDrawData());
}

void RenderContext::renderCursorOverlay(float deltaTime) {
    MW_PROFILE_SCOPE("Overlay.Cursor");

    if (!imguiContext_) {
        return;
    }

    ImGui::SetCurrentContext(imguiContext_);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(windowWidth_), static_cast<float>(windowHeight_));
    io.DisplayFramebufferScale = ImVec2(framebufferScaleX_, framebufferScaleY_);
    io.DeltaTime = deltaTime > 0.0f ? deltaTime : 1.0f / 60.0f;

    ImGui::NewFrame();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
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

    ImGui::Render();
    renderImGuiDrawData(ImGui::GetDrawData());
}

void RenderContext::buildChunkMesh(const ClientWorld& world, glm::ivec3 chunkPos, CachedChunkMesh& outMesh) {
    MW_PROFILE_SCOPE("Chunk.BuildMesh");

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
                    if (world.getBlock(worldPos + face.normal).type != BlockType::Air) {
                        continue;
                    }

                    if (vertices.size() > UINT16_MAX - 4) {
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
}

void RenderContext::renderImGuiDrawData(ImDrawData* drawData) {
    MW_PROFILE_SCOPE("Render.ImGuiDrawData");

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

    const bgfx::ProgramHandle program{imguiProgramIndex_};
    const bgfx::TextureHandle fontTexture{imguiFontTextureIndex_};
    const bgfx::UniformHandle textureUniform{imguiTextureUniformIndex_};
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

void RenderContext::invalidateChunkCache(glm::ivec3 chunkPos) {
    MW_PROFILE_COUNTER("Chunk.CacheInvalidations", 1);

    static const std::array<glm::ivec3, 7> kInvalidateOffsets = {{glm::ivec3(0, 0, 0), glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0),
                                                                  glm::ivec3(0, 1, 0), glm::ivec3(0, -1, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1)}};
    for (const auto& off : kInvalidateOffsets) {
        const glm::ivec3 pos = chunkPos + off;
        auto it = chunkMeshCache_.find(pos);
        if (it != chunkMeshCache_.end()) {
            chunkMeshCache_.erase(it);
        }
        chunkBlockCounts_.erase(pos);
    }
}
