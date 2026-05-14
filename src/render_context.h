#pragma once

#include <glm/glm.hpp>

class ClientWorld;
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

    GLFWwindow* window() const { return window_; }

private:
    glm::vec3 forward() const;
    glm::vec3 right() const;
    bool loadShaders();
    void destroyShaders();

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
    unsigned short programIndex_ = 0xffff;
};
