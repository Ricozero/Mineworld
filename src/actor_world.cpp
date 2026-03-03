#include "actor_world.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

#include "chunk.h"
#include "entity.h"

entt::entity ActorWorld::createPlayer(const std::string& name, glm::vec3 position) {
    if (getEntityByName(name) != entt::null) {
        spdlog::error("Player '{}' already exists", name);
        return entt::null;
    }
    auto entity = registry_.create();
    registry_.emplace<NameComponent>(entity, name);
    registry_.emplace<TransformComponent>(entity, position);
    registry_.emplace<PhysicsComponent>(entity);
    registry_.emplace<BoxColliderComponent>(entity);
    registry_.emplace<PlayerComponent>(entity);
    nameToEntityMap_[name] = entity;
    updateEntityChunk(entity, position);
    spdlog::info("Player '{}' created at ({}, {}, {})", name, position.x, position.y, position.z);
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
    return worldPos / Chunk::SIZE;
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
