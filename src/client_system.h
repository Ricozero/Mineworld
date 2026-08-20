#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "common_system.h"

class ClientWorld;
class ClientChunkManager;
class RenderContext;

class InputSystem : public common_system::BaseSystem<ClientWorld> {
public:
    InputSystem(RenderContext* renderContext, uint32_t localSessionId);
    void update(ClientWorld& world, float deltaTime) override;

private:
    RenderContext* renderContext_ = nullptr;
    uint32_t localSessionId_ = 0;
};

class RenderSystem : public common_system::BaseSystem<ClientWorld> {
public:
    RenderSystem(RenderContext* renderContext, const ClientChunkManager* chunkManager, uint32_t localSessionId);
    void update(ClientWorld& world, float deltaTime) override;

private:
    RenderContext* renderContext_ = nullptr;
    const ClientChunkManager* chunkManager_ = nullptr;
    uint32_t localSessionId_ = 0;
};
