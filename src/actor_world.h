#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "entity.h"

class ActorWorld {
public:
    entt::registry& registry() { return registry_; }
    const entt::registry& registry() const { return registry_; }

    entt::entity createPlayer(
        const std::string& name,
        uint32_t sessionId,
        glm::vec3 position = glm::vec3(0.0f),
        PlayerMode mode = PlayerMode::Survival);
    entt::entity createRemotePlayer(
        const std::string& name,
        glm::vec3 position = glm::vec3(0.0f),
        PlayerMode mode = PlayerMode::Survival);
    entt::entity createRobot(const std::string& name, glm::vec3 position = glm::vec3(0.0f));
    void destroyEntity(entt::entity entity);
    entt::entity getEntityByName(const std::string& name) const;
    void setPlayerMode(entt::entity entity, PlayerMode mode);

    void updateEntityChunk(entt::entity entity, const glm::vec3& position);
    glm::ivec3 getEntityChunk(entt::entity entity) const;
    const std::vector<entt::entity>& getEntitiesInChunk(glm::ivec3 chunkPos) const;
    bool loadEntitiesInChunk(glm::ivec3 chunkPos);
    bool unloadEntitiesInChunk(glm::ivec3 chunkPos);

private:
    entt::entity createPlayerEntity(
        const std::string& name,
        std::optional<uint32_t> sessionId,
        glm::vec3 position,
        PlayerMode mode);
    void applyPlayerModeComponents(entt::entity entity);
    static glm::ivec3 positionToChunk(const glm::vec3& position);
    void addEntityToChunk(entt::entity entity, glm::ivec3 chunkPos);
    void removeEntityFromChunk(entt::entity entity, glm::ivec3 chunkPos);

    entt::registry registry_;
    std::unordered_map<std::string, entt::entity> nameToEntityMap_;
    std::unordered_map<glm::ivec3, std::vector<entt::entity>> chunkToEntities_;
    std::unordered_map<entt::entity, glm::ivec3> entityToChunk_;
};
