#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "block.h"

class World;

class BaseSystem {
public:
    virtual ~BaseSystem() = default;
    virtual void update(World& world, float deltaTime) = 0;
};

class InputSystem : public BaseSystem {
public:
    void update(World& world, float deltaTime) override;

private:
    void updatePlayerInput(entt::registry& registry, float deltaTime);
};

class PhysicsSystem : public BaseSystem {
public:
    void update(World& world, float deltaTime) override;

private:
    void applyGravity(entt::registry& registry, float deltaTime);
    void updateMovement(entt::registry& registry, float deltaTime);
};

class RenderSystem : public BaseSystem {
public:
    void update(World& world, float deltaTime) override;

private:
    void renderBlock(const glm::ivec3& pos, BlockType type);
    void renderEntity(const std::string& name, const glm::vec3& position, const glm::vec3& color);
};
