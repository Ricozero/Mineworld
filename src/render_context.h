#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <string>

#include "entity.h"

class ClientChunkManager;
class ClientWorld;
struct ImDrawData;
struct ImGuiContext;
struct GLFWwindow;

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
    ConnectingAction renderConnecting(const std::string& address, uint16_t port, const std::string& status, bool failed);

    // In-game loop — called every frame while a session is active
    bool shouldClose() const;
    void pollEvents();
    void processInput(float deltaTime, glm::vec3& rotation, PlayerComponent& player, ControllerInputComponent& input);
    void setCamera(const glm::vec3& position, float yaw, float pitch, PlayerMode mode, uint32_t localSessionId);
    void render(const ClientWorld& world, const ClientChunkManager& chunkManager);

    // In-game menu (ESC) control
    void captureMouse();
    void releaseMouse();
    void resetInGameMenu();
    InGameMenuAction consumeInGameMenuAction();

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
        uint16_t program = 0xffff;
        uint16_t fontTexture = 0xffff;
        uint16_t textureUniform = 0xffff;
    };

    // Init helpers
    bool loadShaders();
    void destroyShaders();
    bool initializeImGui();
    void shutdownImGui();
    void updateDisplayMetrics();

    // Per-frame render helpers (called within a single NewFrame/Render pair)
    void renderWorld(const ClientWorld& world, const ClientChunkManager& chunkManager);
    void renderProfilerOverlay();
    void renderCursorOverlay();
    void renderInGameMenu();
    void renderImGuiDrawData(ImDrawData* drawData);

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
    bool mouseCaptured_ = false;
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
};
