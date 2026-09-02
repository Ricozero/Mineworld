#pragma once

class ActorWorld;
class VoxelWorld;

class System {
public:
    virtual ~System() = default;
    virtual void update(VoxelWorld& voxelWorld, ActorWorld& actorWorld, float deltaTime) = 0;
};
