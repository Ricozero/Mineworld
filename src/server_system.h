#pragma once

#include "system.h"

class PhysicsSystem : public System {
public:
    void update(VoxelWorld& voxelWorld, ActorWorld& actorWorld, float deltaTime) override;

private:
    void updateMovement(VoxelWorld& voxelWorld, ActorWorld& actorWorld, float deltaTime);
};
