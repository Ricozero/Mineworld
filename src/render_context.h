#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "entity.h"

class ClientWorld;
struct ImDrawData;
struct ImGuiContext;
struct GLFWwindow;

class ChunkMeshCache {
public:
    struct Entry {
        std::vector<float> vertexData;
        std::vector<uint16_t> indices;
        size_t vertexCount = 0;
    };

    bool contains(glm::ivec3 chunkPos) const;
    const Entry* get(glm::ivec3 chunkPos) const;
    void put(glm::ivec3 chunkPos, size_t blockCount, Entry entry);
    void invalidate(glm::ivec3 chunkPos);
    void evictStale(const std::vector<glm::ivec3>& loadedChunks);
    bool needsRebuild(glm::ivec3 chunkPos, size_t currentBlockCount,
                      const std::unordered_map<glm::ivec3, size_t>& currentCounts) const;

    const std::unordered_map<glm::ivec3, Entry>& entries() const { return entries_; }
    size_t size() const { return entries_.size(); }

private:
    std::unordered_map<glm::ivec3, Entry> entries_;
    std::unordered_map<glm::ivec3, size_t> blockCounts_;
};

class RenderContext {
public:
    // clang-format off
    enum class StartMenuAction { None, Local, Remote, Quit };
    enum class ConnectingAction { None, Cancel };
    enum class InGameMenuAction { None, ReturnToStart };
    // clang-format on

    RenderContext() = default;
    ~RenderContext();

    RenderContext(const RenderContext&) = delete;
    RenderContext& operator=(const RenderContext&) = delete;

    // Lifecycle
    bool initialize(int width, int height, const char* title);
    void shutdown();

    // Menu screens — each call renders and presents a complete frame
    StartMenuAction renderStartMenu(char* addressBuffer, size_t addressBufferSize, int& port);
    ConnectingAction renderConnecting(const std::string& address, uint16_t port);

    // In-game loop — called every frame while a session is active
    bool shouldClose() const;
    void pollEvents();
    void processInput(float deltaTime, glm::vec3& rotation, PlayerComponent& player, ControllerInputComponent& input);
    void setCamera(const glm::vec3& position, float yaw, float pitch, PlayerMode mode, uint32_t localSessionId);
    void render(const ClientWorld& world);

    // In-game menu (ESC) control
    void captureMouse();
    void releaseMouse();
    void closeInGameMenu();
    InGameMenuAction consumeInGameMenuAction();

    // Called by GameClient when server pushes chunk updates
    void invalidateChunkCache(glm::ivec3 chunkPos);

private:
    // clang-format off
    enum class CursorMode { Hidden, Cross, XYZ, Count };
    enum class CameraViewMode { FirstPerson, ThirdPersonFront, ThirdPersonBack, Count };
    enum class ProfilerMode { Hidden, Summary, Full, Count };
    // clang-format on

    struct WorldShader {
        uint16_t program = 0xffff;
    };

    struct ImGuiShader {
        uint16_t program      = 0xffff;
        uint16_t fontTexture  = 0xffff;
        uint16_t textureUniform = 0xffff;
    };

    // Init helpers
    bool loadShaders();
    void destroyShaders();
    bool initializeImGui();
    void shutdownImGui();

    // Per-frame render helpers (called within a single NewFrame/Render pair)
    void renderWorld(const ClientWorld& world);
    void renderProfilerOverlay();
    void renderCursorOverlay();
    void renderInGameMenu();
    void renderImGuiDrawData(ImDrawData* drawData);
    void buildChunkMesh(const ClientWorld& world, glm::ivec3 chunkPos, ChunkMeshCache::Entry& outMesh);

    // Input helpers
    void updateImGuiInput();
    static void handleScroll(GLFWwindow* window, double xOffset, double yOffset);
    glm::vec3 forward() const;
    glm::vec3 right() const;
    bool shouldHideLocalPlayerModel(const ClientWorld& world, entt::entity entity) const;

    // Window & renderer
    GLFWwindow* window_ = nullptr;
    bool bgfxInitialized_ = false;
    int framebufferWidth_ = 1280;
    int framebufferHeight_ = 720;
    int windowWidth_ = 1280;
    int windowHeight_ = 720;
    float framebufferScaleX_ = 1.0f;
    float framebufferScaleY_ = 1.0f;
    WorldShader worldShader_;

    // Camera
    glm::vec3 cameraPosition_{8.0f, 6.0f, 24.0f};
    float cameraYaw_ = -90.0f;
    float cameraPitch_ = -12.0f;
    CameraViewMode cameraViewMode_ = CameraViewMode::FirstPerson;
    uint32_t localSessionId_ = 0;
    std::chrono::steady_clock::time_point lastRenderTime_{};
    bool hasLastRenderTime_ = false;

    // Mouse & input state
    double lastMouseX_ = 0.0;
    double lastMouseY_ = 0.0;
    bool hasMousePosition_ = false;
    bool mouseCaptured_ = true;
    bool prevEscapeDown_ = false;
    bool prevSpaceDown_ = false;
    bool prevF1Down_ = false;
    bool prevF2Down_ = false;
    bool prevF3Down_ = false;
    bool prevF4Down_ = false;
    bool prevF5Down_ = false;

    // Overlay & in-game menu state
    ProfilerMode profilerMode_ = ProfilerMode::Hidden;
    bool showChunkBounds_ = false;
    CursorMode cursorMode_ = CursorMode::Hidden;
    bool inGameMenuOpen_ = false;
    InGameMenuAction pendingInGameMenuAction_ = InGameMenuAction::None;

    // ImGui
    ImGuiContext* imguiContext_ = nullptr;
    double imguiScrollY_ = 0.0;
    ImGuiShader imguiShader_;

    // Chunk mesh cache
    ChunkMeshCache chunkMeshCache_;
};
