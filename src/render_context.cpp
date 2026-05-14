#include "render_context.h"

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/math.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "client_world.h"
#include "entity.h"
#include "log.h"

namespace {

constexpr bgfx::ViewId kMainView = 0;

struct PosColorVertex {
    float x;
    float y;
    float z;
    uint32_t abgr;

    static bgfx::VertexLayout layout;
};

bgfx::VertexLayout PosColorVertex::layout;

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

}  // namespace

RenderContext::~RenderContext() {
    shutdown();
}

bool RenderContext::initialize(int width, int height, const char* title) {
    width_ = width;
    height_ = height;

    if (!glfwInit()) {
        logging::error("Failed to initialize GLFW");
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window_ = glfwCreateWindow(width_, height_, title, nullptr, nullptr);
    if (!window_) {
        logging::error("Failed to create GLFW window");
        glfwTerminate();
        return false;
    }

    bgfx::Init init;
    init.type = bgfx::RendererType::Direct3D11;
    init.platformData.nwh = glfwGetWin32Window(window_);
    init.resolution.width = static_cast<uint32_t>(width_);
    init.resolution.height = static_cast<uint32_t>(height_);
    init.resolution.reset = BGFX_RESET_VSYNC;
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

    if (!loadShaders()) {
        shutdown();
        return false;
    }

    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    logging::info("Renderer initialized with GLFW/bgfx ({})", bgfx::getRendererName(bgfx::getRendererType()));
    return true;
}

void RenderContext::shutdown() {
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
    glfwPollEvents();
}

void RenderContext::updateCamera(float deltaTime) {
    if (!window_) {
        return;
    }

    deltaTime = std::clamp(deltaTime, 0.0f, 0.05f);

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

    cameraYaw_ += mouseDeltaX * mouseSensitivity;
    cameraPitch_ -= mouseDeltaY * mouseSensitivity;
    cameraPitch_ = std::clamp(cameraPitch_, -88.0f, 88.0f);

    constexpr float moveSpeed = 10.0f;
    constexpr float turnSpeed = 95.0f;
    const float moveStep = moveSpeed * deltaTime;
    const float turnStep = turnSpeed * deltaTime;

    if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) {
        cameraPosition_ += forward() * moveStep;
    }
    if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) {
        cameraPosition_ -= forward() * moveStep;
    }
    if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) {
        cameraPosition_ -= right() * moveStep;
    }
    if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) {
        cameraPosition_ += right() * moveStep;
    }
    if (glfwGetKey(window_, GLFW_KEY_SPACE) == GLFW_PRESS) {
        cameraPosition_.y += moveStep;
    }
    if (glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
        cameraPosition_.y -= moveStep;
    }
    if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }
}

void RenderContext::render(const ClientWorld& world) {
    if (!window_ || !bgfxInitialized_) {
        return;
    }

    glfwGetFramebufferSize(window_, &width_, &height_);
    width_ = std::max(width_, 1);
    height_ = std::max(height_, 1);

    bgfx::setViewRect(kMainView, 0, 0, static_cast<uint16_t>(width_), static_cast<uint16_t>(height_));
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
        static_cast<float>(width_) / height_,
        0.1f,
        500.0f,
        bgfx::getCaps()->homogeneousDepth,
        bx::Handedness::Right);
    bgfx::setViewTransform(kMainView, view, projection);
    bgfx::touch(kMainView);

    MeshBuilder mesh;
    mesh.vertices.reserve(8192);
    mesh.indices.reserve(12288);

    const VoxelWorld& voxelWorld = world.getVoxelWorld();
    for (const glm::ivec3& chunkPos : voxelWorld.getLoadedChunks()) {
        const Chunk& chunk = voxelWorld.getChunk(chunkPos);
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

                        std::array<glm::vec3, 4> corners;
                        for (size_t i = 0; i < face.corners.size(); ++i) {
                            corners[i] = glm::vec3(worldPos) + face.corners[i];
                        }
                        addQuad(mesh, corners, baseColor * face.shade);
                    }
                }
            }
        }
    }

    const auto& registry = world.getActorWorld().registry();
    auto viewEntities = registry.view<TransformComponent, NameComponent>();
    for (auto entity : viewEntities) {
        const auto& transform = registry.get<TransformComponent>(entity);
        const auto& name = registry.get<NameComponent>(entity);
        const glm::vec3 color = name.name == "Steve" ? glm::vec3(0.18f, 0.42f, 0.85f) : glm::vec3(0.85f, 0.32f, 0.20f);
        const glm::vec3 min = transform.position + glm::vec3(-0.35f, 0.0f, -0.35f);
        const glm::vec3 max = transform.position + glm::vec3(0.35f, 1.8f, 0.35f);
        addBox(mesh, min, max, color);
    }

    if (!mesh.vertices.empty() && !mesh.indices.empty()) {
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
            bgfx::submit(kMainView, bgfx::ProgramHandle{programIndex_});
        }
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
