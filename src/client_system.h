#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "common_system.h"

class ClientWorld;
class RenderContext;

class InputSystem : public common_system::BaseSystem<ClientWorld> {
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

class RenderSystem : public common_system::BaseSystem<ClientWorld> {
public:
    RenderSystem(RenderContext* renderContext, uint32_t localSessionId);
    void update(ClientWorld& world, float deltaTime) override;

private:
    RenderContext* renderContext_ = nullptr;
    uint32_t localSessionId_ = 0;
};
