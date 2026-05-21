#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <string>

#include "block.h"

class ClientWorld;
class RenderContext;
class ServerWorld;

class ClientSystem {
public:
    virtual ~ClientSystem() = default;
    virtual void update(ClientWorld& world, float deltaTime) = 0;
};

class ServerSystem {
public:
    virtual ~ServerSystem() = default;
    virtual void update(ServerWorld& world, float deltaTime) = 0;
};

class InputSystem : public ClientSystem {
public:
    explicit InputSystem(RenderContext* renderContext = nullptr, const std::string& spectatorName = "");
    void update(ClientWorld& world, float deltaTime) override;

private:
    void updatePlayerInput(entt::registry& registry, float deltaTime);
    RenderContext* renderContext_ = nullptr;
    std::string spectatorName_;
};

class PhysicsSystem : public ServerSystem {
public:
    void update(ServerWorld& world, float deltaTime) override;

private:
    void applyGravity(entt::registry& registry, float deltaTime);
    void updateMovement(entt::registry& registry, float deltaTime);
};

class RenderSystem : public ClientSystem {
public:
    explicit RenderSystem(RenderContext* renderContext = nullptr);
    void update(ClientWorld& world, float deltaTime) override;

private:
    void renderBlock(const glm::ivec3& pos, BlockType type);
    void renderEntity(const std::string& name, const glm::vec3& position, const glm::vec3& color);
    RenderContext* renderContext_ = nullptr;
};