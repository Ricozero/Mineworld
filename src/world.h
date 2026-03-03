#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <string>

#include "actor_world.h"
#include "voxel_world.h"

class World {
public:
    World();
    ~World();

    VoxelWorld& getVoxelWorld() { return voxelWorld_; }
    const VoxelWorld& getVoxelWorld() const { return voxelWorld_; }

    ActorWorld& getActorWorld() { return actorWorld_; }
    const ActorWorld& getActorWorld() const { return actorWorld_; }

    Chunk& getChunk(glm::ivec3 chunkPos);
    BlockData getBlock(glm::ivec3 worldPos) const;
    void setBlock(glm::ivec3 worldPos, BlockData blockData);

    bool loadChunk(glm::ivec3 chunkPos);
    bool unloadChunk(glm::ivec3 chunkPos);

    entt::registry& getRegistry() { return actorWorld_.registry(); }
    const entt::registry& getRegistry() const { return actorWorld_.registry(); }

    entt::entity createPlayer(const std::string& name, glm::vec3 position = glm::vec3(0.0f));
    void destroyEntity(entt::entity entity);

    entt::entity getEntityByName(const std::string& name) const;

private:
    VoxelWorld voxelWorld_;
    ActorWorld actorWorld_;
};
