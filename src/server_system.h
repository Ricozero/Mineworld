#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "common_system.h"

class ServerWorld;

class PhysicsSystem : public common_system::BaseSystem<ServerWorld> {
public:
    void update(ServerWorld& world, float deltaTime) override;

private:
    void updateMovement(ServerWorld& world, float deltaTime);
};
