#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

class ServerWorld;

void applyControllerInput(entt::registry& registry, entt::entity entity, float deltaTime, bool consumeJump);
void simulateServerActor(ServerWorld& world, entt::registry& registry, entt::entity entity, float deltaTime);

class ServerSystem {
public:
    virtual ~ServerSystem() = default;
    virtual void update(ServerWorld& world, float deltaTime) = 0;
};

class PhysicsSystem : public ServerSystem {
public:
    void update(ServerWorld& world, float deltaTime) override;

private:
    void applyGravity(entt::registry& registry, float deltaTime);
    void updateMovement(ServerWorld& world, float deltaTime);
    void moveWithCollision(ServerWorld& world, entt::entity entity, float deltaTime);
};
