#include "world.h"

#include <print>

#include "entity.h"
#include "system.h"

World::World() {
    registerSystem(std::make_unique<InputSystem>());
    registerSystem(std::make_unique<PhysicsSystem>());
    registerSystem(std::make_unique<RenderSystem>());
}

World::~World() = default;

Chunk& World::getOrCreateChunk(glm::ivec3 chunkPos) {
    auto it = chunks_.find(chunkPos);
    if (it != chunks_.end()) {
        return *(it->second);
    }
    auto& chunk = chunks_[chunkPos] = std::make_unique<Chunk>(chunkPos);
    return *chunk;
}

BlockData World::getBlock(glm::ivec3 worldPos) const {
    glm::ivec3 localPos = Chunk::worldToLocal(worldPos);
    auto it = chunks_.find(worldPos / Chunk::SIZE);
    if (it == chunks_.end()) {
        return BlockData{BlockType::Air, BlockOrientation::North};
    }
    return it->second->getBlock(localPos);
}

void World::setBlock(glm::ivec3 worldPos, BlockData blockData) {
    glm::ivec3 localPos = Chunk::worldToLocal(worldPos);
    auto& chunk = getOrCreateChunk(worldPos / Chunk::SIZE);
    chunk.setBlock(localPos, blockData);
}

entt::entity World::createPlayer(const std::string& name, glm::vec3 position) {
    if (getEntityByName(name) != entt::null) {
        std::println(stderr, "[World] Player '{}' already exists", name);
        return entt::null;
    }
    auto entity = registry_.create();
    registry_.emplace<NameComponent>(entity, name);
    registry_.emplace<TransformComponent>(entity, position);
    registry_.emplace<PhysicsComponent>(entity);
    registry_.emplace<BoxColliderComponent>(entity);
    registry_.emplace<PlayerComponent>(entity);
    nameToEntityMap_[name] = entity;
    std::println("[World] Player '{}' created at ({}, {}, {})", name, position.x, position.y, position.z);
    return entity;
}

void World::destroyEntity(entt::entity entity) {
    if (registry_.all_of<NameComponent>(entity)) {
        auto& nameComp = registry_.get<NameComponent>(entity);
        nameToEntityMap_.erase(nameComp.name);
    }
    registry_.destroy(entity);
}

entt::entity World::getEntityByName(const std::string& name) const {
    auto it = nameToEntityMap_.find(name);
    return (it != nameToEntityMap_.end()) ? it->second : entt::null;
}

void World::registerSystem(std::unique_ptr<BaseSystem> system) {
    systems_.push_back(std::move(system));
}

void World::update(float deltaTime) {
    for (auto& system : systems_) {
        system->update(*this, deltaTime);
    }
}
