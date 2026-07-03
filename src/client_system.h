#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

class ClientWorld;
class RenderContext;

class ClientSystem {
public:
    virtual ~ClientSystem() = default;
    virtual void update(ClientWorld& world, float deltaTime) = 0;
};

class InputSystem : public ClientSystem {
public:
    InputSystem(RenderContext* renderContext, uint32_t localSessionId);
    void update(ClientWorld& world, float deltaTime) override;

    bool hasInputChanged() const { return inputChanged_; }
    void clearInputChanged() { inputChanged_ = false; }
    bool hasPendingInput() const { return pendingInput_; }
    void clearPendingInput() { pendingInput_ = false; }

private:
    RenderContext* renderContext_ = nullptr;
    uint32_t localSessionId_ = 0;
    bool inputChanged_ = false;
    bool pendingInput_ = false;
};

class RenderSystem : public ClientSystem {
public:
    RenderSystem(RenderContext* renderContext, uint32_t localSessionId);
    void update(ClientWorld& world, float deltaTime) override;

private:
    RenderContext* renderContext_ = nullptr;
    uint32_t localSessionId_ = 0;
};
