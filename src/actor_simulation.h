#pragma once

#include <entt/entt.hpp>

class VoxelWorld;

namespace actor_simulation {

void applyControllerInput(entt::registry& registry, entt::entity entity, float deltaTime, bool consumeJump);
void refreshGrounded(const VoxelWorld& voxelWorld, entt::registry& registry, entt::entity entity);
void simulatePhysics(const VoxelWorld& voxelWorld, entt::registry& registry, entt::entity entity, float deltaTime);

}  // namespace actor_simulation
