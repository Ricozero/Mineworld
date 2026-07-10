#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <string>
#include <vector>

#include "actor_world.h"
#include "voxel_world.h"

class ServerWorld {
public:
    ServerWorld() = default;
    ~ServerWorld() = default;

    VoxelWorld& getVoxelWorld() { return voxelWorld_; }
    const VoxelWorld& getVoxelWorld() const { return voxelWorld_; }

    ActorWorld& getActorWorld() { return actorWorld_; }
    const ActorWorld& getActorWorld() const { return actorWorld_; }

    Chunk& getChunk(glm::ivec3 chunkPos);
    BlockData getBlock(glm::ivec3 worldPos) const;
    void setBlock(glm::ivec3 worldPos, BlockData blockData);

    bool loadChunk(glm::ivec3 chunkPos);
    bool unloadChunk(glm::ivec3 chunkPos);
    bool isChunkInBounds(glm::ivec3 chunkPos) const;

    std::vector<glm::ivec3> getLoadedChunks() const;

    entt::entity createLocalPlayer(const std::string& name, uint32_t sessionId, glm::vec3 position, PlayerMode mode);
    entt::entity createRobot(const std::string& name, glm::vec3 position);
    void destroyEntity(entt::entity entity);
    entt::entity getEntityByName(const std::string& name) const;

private:
    void generateChunk(Chunk& chunk) const;
    BlockData generateBlock(glm::ivec3 worldPos) const;
    bool isBlockInBounds(glm::ivec3 worldPos) const;

    VoxelWorld voxelWorld_;
    ActorWorld actorWorld_{true};
};
