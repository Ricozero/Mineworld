#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

class ServerWorld;

class ServerSystem {
public:
    virtual ~ServerSystem() = default;
    virtual void update(ServerWorld& world, float deltaTime) = 0;
};

class PhysicsSystem : public ServerSystem {
public:
    void update(ServerWorld& world, float deltaTime) override;

private:
    void updateMovement(ServerWorld& world, float deltaTime);
};
