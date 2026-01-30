#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "block.h"

class World;

// ============ 系统基类 ============

class BaseSystem {
public:
    virtual ~BaseSystem() = default;
    virtual void update(World& world, float deltaTime) = 0;
};

// ============ 物理系统 ============

class PhysicsSystem : public BaseSystem {
public:
    void update(World& world, float deltaTime) override;

private:
    void applyGravity(entt::registry& registry, float deltaTime);
    void updateMovement(entt::registry& registry, float deltaTime);
    bool checkCollision(const glm::vec3& position);
};

// ============ 渲染系统 ============

class RenderSystem : public BaseSystem {
public:
    void update(World& world, float deltaTime) override;

private:
    void renderEntity(const std::string& name, const glm::vec3& position,
                      const glm::vec3& color);
    void renderBlock(const glm::ivec3& pos, BlockType type);
};

// ============ 生命周期系统 ============

class LifeSystem : public BaseSystem {
public:
    void update(World& world, float deltaTime) override;

private:
    void updateHealth(entt::registry& registry, float deltaTime);
};

// ============ 游戏逻辑系统 ============

class GameLogicSystem : public BaseSystem {
public:
    void update(World& world, float deltaTime) override;

private:
    void updatePlayerInput(entt::registry& registry, float deltaTime);
    void checkWorldBoundaries(entt::registry& registry);
};