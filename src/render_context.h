#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <vector>

#include "entity.h"
#include "input_history.h"
#include "orientation.h"
#include "text.h"

class ClientChunkManager;
class ChunkCuller;
class ActorWorld;
struct Frustum;
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
    bool initialize(int width, int height, const char* title, const std::string& baseDir);
    void shutdown();

    // Menu screens
    StartMenuAction renderStartMenu(char* addressBuffer, size_t addressBufferSize, int& port);
    ConnectingAction renderConnecting(const std::string& address, uint16_t port, const std::string& status, bool failed);

    // In-game loop
    bool shouldClose() const;
    void pollEvents();
    void beginFrame();
    void endFrame();
    void processInput(glm::vec3& rotation, PlayerComponent& player, ControllerInputComponent& input);
    std::optional<std::string> consumeConsoleInput();
    void appendConsoleLine(std::string text, bool success = true);
    void setGameActive(bool active) { gameActive_ = active; }
    void setCamera(const glm::vec3& position, float yaw, float pitch, PlayerMode mode, uint32_t localSessionId);
    void render(const ActorWorld& actorWorld, ClientChunkManager& chunkManager, ChunkCuller& chunkCuller);

    // In-game menu
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

    struct ShaderProgram {
        uint16_t program = 0xffff;
    };

    struct ImGuiShader {
        uint16_t program = 0xffff;
        uint16_t fontTexture = 0xffff;
        uint16_t textureUniform = 0xffff;
    };

    // Init helpers
    bool loadProgram(const char* vertexName, const char* fragmentName, uint16_t& program) const;
    bool loadShaders();
    void destroyShaders();
    bool initializeImGui();
    void shutdownImGui();
    void updateDisplayMetrics();

    // Per-frame render helpers
    void renderWorld(const ActorWorld& actorWorld, const ClientChunkManager& chunkManager, ChunkCuller& chunkCuller, const Frustum& frustum);
    void renderEntityNames(const ActorWorld& actorWorld, const float* viewProjection);
    void renderProfilerOverlay();
    void renderCursorOverlay();
    void renderInGameMenu();
    void renderConsole();
    void renderImGuiDrawData(ImDrawData* drawData);

    // Input helpers
    void openConsole(bool command);
    orientation::Basis cameraBasis() const;
    bool shouldHideLocalPlayerModel(const ActorWorld& actorWorld, entt::entity entity) const;

    // Window & renderer
    std::string baseDir_;
    std::vector<uint8_t> fontData_;
    std::vector<uint8_t> fallbackFontData_;
    GLFWwindow* window_ = nullptr;
    bool bgfxInitialized_ = false;
    int framebufferWidth_ = 1280;
    int framebufferHeight_ = 720;
    int windowWidth_ = 1280;
    int windowHeight_ = 720;
    ShaderProgram unlitShader_;
    ShaderProgram chunkShader_;

    // Camera
    glm::vec3 cameraPosition_{8.0f, 6.0f, 24.0f};
    float cameraYaw_ = -90.0f;
    float cameraPitch_ = -12.0f;
    CameraViewMode cameraViewMode_ = CameraViewMode::FirstPerson;
    uint32_t localSessionId_ = 0;

    // Mouse & input state
    double lastMouseX_ = 0.0;
    double lastMouseY_ = 0.0;
    bool hasMousePosition_ = false;
    bool mouseCaptured_ = false;
    bool gameActive_ = false;

    // In-game console
    struct ConsoleState {
        struct Line {
            std::string text;
            bool success;
        };

        bool open = false;
        bool focusInput = false;
        bool positionCursor = false;
        unsigned int openingCharacter = 0;
        InputHistory history;
        std::array<char, kMaxInputTextBytes + 1> input{};
        std::deque<std::string> pendingInputs;
        std::deque<Line> lines;
        std::chrono::steady_clock::time_point lastActivityTime{};
        bool scrollToBottom = false;
    };
    ConsoleState console_;

    // Overlay & in-game menu state
    ProfilerMode profilerMode_ = ProfilerMode::Hidden;
    bool showChunkBounds_ = false;
    CursorMode cursorMode_ = CursorMode::Hidden;
    bool inGameMenuOpen_ = false;
    InGameMenuAction pendingInGameMenuAction_ = InGameMenuAction::None;

    // ImGui
    ImGuiContext* imguiContext_ = nullptr;
    bool imguiFrameActive_ = false;
    ImGuiShader imguiShader_;
};
