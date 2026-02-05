#pragma once

#define GLM_ENABLE_EXPERIMENTAL

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

#include "block.h"
#include "chunk.h"

class BaseSystem;

class World {
public:
    World();
    ~World();

    Chunk& getOrCreateChunk(glm::ivec3 chunkPos);

    BlockData getBlock(glm::ivec3 worldPos) const;
    void setBlock(glm::ivec3 worldPos, BlockData blockData);

    entt::registry& getRegistry() { return registry_; }
    const entt::registry& getRegistry() const { return registry_; }

    entt::entity createPlayer(const std::string& name, glm::vec3 position = glm::vec3(0.0f));
    void destroyEntity(entt::entity entity);

    entt::entity getEntityByName(const std::string& name) const;

    void registerSystem(std::unique_ptr<BaseSystem> system);
    void update(float deltaTime);

private:
    entt::registry registry_;
    std::vector<std::unique_ptr<BaseSystem>> systems_;
    std::unordered_map<std::string, entt::entity> nameToEntityMap_;
    std::unordered_map<glm::ivec3, std::unique_ptr<Chunk>> chunks_;
};
