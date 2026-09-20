#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "entity.h"

class ActorWorld {
public:
    ActorWorld();
    ~ActorWorld();

    entt::registry& registry() { return registry_; }
    const entt::registry& registry() const { return registry_; }

    entt::entity createActor(ActorId id, glm::vec3 position);
    entt::entity createLocalPlayer(ActorId id, std::string_view name, uint32_t sessionId, glm::vec3 position, PlayerMode mode);
    entt::entity createRemotePlayer(ActorId id, std::string_view name, glm::vec3 position, PlayerMode mode);
    entt::entity createRobot(ActorId id, std::string_view name, glm::vec3 position);
    bool destroyActor(ActorId id);
    void destroyEntity(entt::entity entity);
    entt::entity getEntity(ActorId id) const;
    std::vector<ActorId> findActorsByName(std::string_view name) const;
    void setName(entt::entity entity, std::string_view name);
    void setPlayerMode(entt::entity entity, PlayerMode mode);

private:
    entt::entity createPlayerEntity(ActorId id, std::string_view name, std::optional<uint32_t> sessionId, glm::vec3 position, PlayerMode mode);
    void onActorDestroyed(entt::registry& registry, entt::entity entity);

    entt::registry registry_;
    std::unordered_map<ActorId, entt::entity> idToEntity_;
};
