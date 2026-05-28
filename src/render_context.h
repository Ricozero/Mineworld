#pragma once

#include <chrono>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <unordered_map>
#include <vector>

class ClientWorld;
struct ImDrawData;
struct ImGuiContext;
struct GLFWwindow;

class RenderContext {
public:
    RenderContext() = default;
    ~RenderContext();

    RenderContext(const RenderContext&) = delete;
    RenderContext& operator=(const RenderContext&) = delete;

    bool initialize(int width, int height, const char* title);
    void shutdown();

    bool shouldClose() const;
    void pollEvents();
    void updateCamera(float deltaTime);

    void render(const ClientWorld& world);
    void invalidateChunkCache(glm::ivec3 chunkPos);

    GLFWwindow* window() const { return window_; }
    glm::vec3 getCameraPosition() const { return cameraPosition_; }

private:
    struct CachedChunkMesh {
        std::vector<float> vertexData;
        std::vector<uint16_t> indices;
        size_t vertexCount = 0;
    };

    glm::vec3 forward() const;
    glm::vec3 right() const;
    bool loadShaders();
    void destroyShaders();
    bool initializeImGui();
    void shutdownImGui();

    void renderWorld(const ClientWorld& world);
    void renderProfilerOverlay(float deltaTime);
    void renderCursorOverlay(float deltaTime);
    void renderImGuiDrawData(ImDrawData* drawData);
    void buildChunkMesh(const ClientWorld& world, glm::ivec3 chunkPos, CachedChunkMesh& outMesh);

    GLFWwindow* window_ = nullptr;
    int width_ = 1280;
    int height_ = 720;
    glm::vec3 cameraPosition_{8.0f, 6.0f, 24.0f};
    float cameraYaw_ = -90.0f;
    float cameraPitch_ = -12.0f;
    double lastMouseX_ = 0.0;
    double lastMouseY_ = 0.0;
    bool hasMousePosition_ = false;
    bool bgfxInitialized_ = false;
    bool showProfiler_ = false;
    enum class CursorMode { None,
                            Cross,
                            XYZ };
    CursorMode cursorMode_ = CursorMode::None;
    bool showChunkBounds_ = false;
    bool prevF1Down_ = false;
    bool prevF2Down_ = false;
    bool prevF3Down_ = false;
    unsigned short programIndex_ = 0xffff;
    unsigned short imguiProgramIndex_ = 0xffff;
    unsigned short imguiFontTextureIndex_ = 0xffff;
    unsigned short imguiTextureUniformIndex_ = 0xffff;
    ImGuiContext* imguiContext_ = nullptr;

    std::chrono::steady_clock::time_point lastRenderTime_{};
    bool hasLastRenderTime_ = false;

    std::unordered_map<glm::ivec3, CachedChunkMesh> chunkMeshCache_;
    std::unordered_map<glm::ivec3, size_t> chunkBlockCounts_;
};