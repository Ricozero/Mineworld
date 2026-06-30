#include "actor_world.h"

#include <algorithm>
#include <cmath>

#include "chunk.h"
#include "entity.h"
#include "log.h"

namespace {

BoxColliderComponent createPlayerCollider() {
    BoxColliderComponent collider;
    collider.offset = glm::vec3(0.0f, 0.9f, 0.0f);
    collider.size = glm::vec3(0.7f, 1.8f, 0.7f);
    return collider;
}

}  // namespace

entt::entity ActorWorld::createLocalPlayer(const std::string& name, uint32_t sessionId, glm::vec3 position, PlayerMode mode) {
    return createPlayerEntity(name, sessionId, position, mode);
}

entt::entity ActorWorld::createRemotePlayer(const std::string& name, glm::vec3 position, PlayerMode mode) {
    return createPlayerEntity(name, std::nullopt, position, mode);
}

entt::entity ActorWorld::createPlayerEntity(const std::string& name, std::optional<uint32_t> sessionId, glm::vec3 position, PlayerMode mode) {
    if (getEntityByName(name) != entt::null) {
        logging::error("Player '{}' already exists", name);
        return entt::null;
    }
    auto entity = registry_.create();
    registry_.emplace<NameComponent>(entity, name);
    registry_.emplace<TransformComponent>(entity, position);
    PlayerComponent player;
    player.mode = mode;
    registry_.emplace<PlayerComponent>(entity, player);
    registry_.emplace<ControllerInputComponent>(entity);
    registry_.emplace<MeshComponent>(entity, glm::vec4(0.18f, 0.42f, 0.85f, 1.0f), mode == PlayerMode::Survival);
    if (sessionId.has_value()) {
        registry_.emplace<SessionComponent>(entity, *sessionId);
    }
    applyPlayerModeComponents(entity);
    nameToEntityMap_[name] = entity;
    updateEntityChunk(entity, position);
    if (sessionId.has_value()) {
        logging::info("Player '{}' created at ({}, {}, {}) mode={} session={}",
                      name, position.x, position.y, position.z, mode == PlayerMode::Spectator ? "spectator" : "survival", *sessionId);
    } else {
        logging::info("Remote player '{}' created at ({}, {}, {}) mode={}",
                      name, position.x, position.y, position.z, mode == PlayerMode::Spectator ? "spectator" : "survival");
    }
    return entity;
}

entt::entity ActorWorld::createRobot(const std::string& name, glm::vec3 position) {
    if (getEntityByName(name) != entt::null) {
        logging::error("Robot '{}' already exists", name);
        return entt::null;
    }
    auto entity = registry_.create();
    registry_.emplace<NameComponent>(entity, name);
    registry_.emplace<TransformComponent>(entity, position);
    registry_.emplace<PhysicsComponent>(entity);
    registry_.emplace<BoxColliderComponent>(entity, createPlayerCollider());
    registry_.emplace<RobotComponent>(entity);
    registry_.emplace<RandomMovementComponent>(entity);
    registry_.emplace<ControllerInputComponent>(entity);
    registry_.emplace<MeshComponent>(entity, glm::vec4(0.85f, 0.32f, 0.20f, 1.0f), true);
    nameToEntityMap_[name] = entity;
    updateEntityChunk(entity, position);
    logging::info("Robot '{}' created at ({}, {}, {})", name, position.x, position.y, position.z);
    return entity;
}

void ActorWorld::destroyEntity(entt::entity entity) {
    if (registry_.all_of<NameComponent>(entity)) {
        auto& nameComp = registry_.get<NameComponent>(entity);
        nameToEntityMap_.erase(nameComp.name);
    }

    auto it = entityToChunk_.find(entity);
    if (it != entityToChunk_.end()) {
        removeEntityFromChunk(entity, it->second);
        entityToChunk_.erase(it);
    }

    registry_.destroy(entity);
}

entt::entity ActorWorld::getEntityByName(const std::string& name) const {
    auto it = nameToEntityMap_.find(name);
    return (it != nameToEntityMap_.end()) ? it->second : entt::null;
}

void ActorWorld::setPlayerMode(entt::entity entity, PlayerMode mode) {
    if (!registry_.valid(entity) || !registry_.all_of<PlayerComponent>(entity)) {
        return;
    }

    auto& player = registry_.get<PlayerComponent>(entity);
    player.mode = mode;
    applyPlayerModeComponents(entity);
}

void ActorWorld::applyPlayerModeComponents(entt::entity entity) {
    if (!registry_.valid(entity) || !registry_.all_of<PlayerComponent>(entity)) {
        return;
    }

    const auto& player = registry_.get<PlayerComponent>(entity);
    if (player.mode == PlayerMode::Spectator) {
        if (registry_.all_of<PhysicsComponent>(entity)) {
            registry_.remove<PhysicsComponent>(entity);
        }
        if (registry_.all_of<BoxColliderComponent>(entity)) {
            registry_.remove<BoxColliderComponent>(entity);
        }
        if (registry_.all_of<MeshComponent>(entity)) {
            registry_.get<MeshComponent>(entity).isVisible = false;
        }
    } else {
        if (!registry_.all_of<PhysicsComponent>(entity)) {
            registry_.emplace<PhysicsComponent>(entity);
        } else {
            auto& physics = registry_.get<PhysicsComponent>(entity);
            physics.velocity = glm::vec3(0.0f);
            physics.acceleration = glm::vec3(0.0f);
            physics.isGrounded = false;
        }
        registry_.emplace_or_replace<BoxColliderComponent>(entity, createPlayerCollider());
        if (registry_.all_of<MeshComponent>(entity)) {
            registry_.get<MeshComponent>(entity).isVisible = true;
        }
    }
}

void ActorWorld::updateEntityChunk(entt::entity entity, const glm::vec3& position) {
    glm::ivec3 newChunk = positionToChunk(position);
    auto it = entityToChunk_.find(entity);
    if (it != entityToChunk_.end()) {
        if (it->second == newChunk) {
            return;
        }
        removeEntityFromChunk(entity, it->second);
        it->second = newChunk;
    } else {
        entityToChunk_[entity] = newChunk;
    }

    addEntityToChunk(entity, newChunk);
}

glm::ivec3 ActorWorld::getEntityChunk(entt::entity entity) const {
    auto it = entityToChunk_.find(entity);
    if (it == entityToChunk_.end()) {
        return glm::ivec3(0);
    }
    return it->second;
}

const std::vector<entt::entity>& ActorWorld::getEntitiesInChunk(glm::ivec3 chunkPos) const {
    static const std::vector<entt::entity> empty;
    auto it = chunkToEntities_.find(chunkPos);
    return it != chunkToEntities_.end() ? it->second : empty;
}

bool ActorWorld::loadEntitiesInChunk(glm::ivec3 chunkPos) {
    return true;
}

bool ActorWorld::unloadEntitiesInChunk(glm::ivec3 chunkPos) {
    auto it = chunkToEntities_.find(chunkPos);
    if (it == chunkToEntities_.end()) {
        return true;
    }

    auto entities = it->second;
    for (auto entity : entities) {
        // Don't destroy session-owned players when their chunk unloads.
        if (registry_.all_of<SessionComponent>(entity)) {
            continue;
        }
        destroyEntity(entity);
    }
    return true;
}

glm::ivec3 ActorWorld::positionToChunk(const glm::vec3& position) {
    glm::ivec3 worldPos{
        static_cast<int>(std::floor(position.x)),
        static_cast<int>(std::floor(position.y)),
        static_cast<int>(std::floor(position.z)),
    };
    return Chunk::worldToChunk(worldPos);
}

void ActorWorld::addEntityToChunk(entt::entity entity, glm::ivec3 chunkPos) {
    auto& list = chunkToEntities_[chunkPos];
    list.push_back(entity);
}

void ActorWorld::removeEntityFromChunk(entt::entity entity, glm::ivec3 chunkPos) {
    auto it = chunkToEntities_.find(chunkPos);
    if (it == chunkToEntities_.end()) {
        return;
    }
    auto& list = it->second;
    list.erase(std::remove(list.begin(), list.end(), entity), list.end());
    if (list.empty()) {
        chunkToEntities_.erase(it);
    }
}
