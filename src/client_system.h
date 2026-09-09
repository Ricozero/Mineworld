#pragma once

#include <cstdint>

#include "system.h"

class ClientChunkManager;
class ChunkCuller;
class RenderContext;

class InputSystem : public System {
public:
    InputSystem(RenderContext* renderContext, uint32_t localSessionId);
    void update(VoxelWorld& voxelWorld, ActorWorld& actorWorld, float deltaTime) override;

private:
    RenderContext* renderContext_ = nullptr;
    uint32_t localSessionId_ = 0;
};

class RenderSystem : public System {
public:
    RenderSystem(RenderContext* renderContext, ClientChunkManager& chunkManager, ChunkCuller& chunkCuller, uint32_t localSessionId);
    void update(VoxelWorld& voxelWorld, ActorWorld& actorWorld, float deltaTime) override;

private:
    RenderContext* renderContext_ = nullptr;
    ClientChunkManager& chunkManager_;
    ChunkCuller& chunkCuller_;
    uint32_t localSessionId_ = 0;
};
