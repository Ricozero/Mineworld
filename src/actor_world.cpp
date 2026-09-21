#include "actor_world.h"

namespace {

BoxColliderComponent createPlayerCollider() {
    BoxColliderComponent collider;
    collider.offset = glm::vec3(0.0f, 0.9f, 0.0f);
    collider.size = glm::vec3(0.7f, 1.8f, 0.7f);
    return collider;
}

}  // namespace

entt::entity ActorWorld::createActor(ActorId id, glm::vec3 position) {
    if (id == 0 || idToEntity_.contains(id)) {
        return entt::null;
    }
    const auto entity = registry_.create();
    registry_.emplace<ActorComponent>(entity, id);
    registry_.emplace<TransformComponent>(entity, position);
    idToEntity_.emplace(id, entity);
    return entity;
}

entt::entity ActorWorld::createLocalPlayer(ActorId id, std::string_view name, uint32_t sessionId, glm::vec3 position, PlayerMode mode) {
    return createPlayerEntity(id, name, sessionId, position, mode);
}

entt::entity ActorWorld::createRemotePlayer(ActorId id, std::string_view name, glm::vec3 position, PlayerMode mode) {
    return createPlayerEntity(id, name, std::nullopt, position, mode);
}

entt::entity ActorWorld::createPlayerEntity(ActorId id, std::string_view name, std::optional<uint32_t> sessionId, glm::vec3 position, PlayerMode mode) {
    const auto entity = createActor(id, position);
    if (entity == entt::null) {
        return entity;
    }
    setName(entity, name);
    PlayerComponent player;
    player.mode = mode;
    registry_.emplace<PlayerComponent>(entity, player);
    registry_.emplace<ControllerInputComponent>(entity);
    registry_.emplace<MeshComponent>(entity, glm::vec4(0.18f, 0.42f, 0.85f, 1.0f), mode != PlayerMode::Spectator);
    if (sessionId) {
        registry_.emplace<SessionComponent>(entity, *sessionId);
    }
    if (mode != PlayerMode::Spectator) {
        registry_.emplace<PhysicsComponent>(entity);
        registry_.emplace<BoxColliderComponent>(entity, createPlayerCollider());
    }
    return entity;
}

entt::entity ActorWorld::createRobot(ActorId id, std::string_view name, glm::vec3 position) {
    const auto entity = createActor(id, position);
    if (entity == entt::null) {
        return entity;
    }
    setName(entity, name);
    registry_.emplace<PhysicsComponent>(entity);
    registry_.emplace<BoxColliderComponent>(entity, createPlayerCollider());
    registry_.emplace<RobotComponent>(entity);
    registry_.emplace<RandomMovementComponent>(entity);
    registry_.emplace<ControllerInputComponent>(entity);
    registry_.emplace<MeshComponent>(entity, glm::vec4(0.85f, 0.32f, 0.20f, 1.0f), true);
    return entity;
}

bool ActorWorld::destroyActor(ActorId id) {
    const auto entity = getEntity(id);
    if (entity == entt::null) {
        return false;
    }
    destroyEntity(entity);
    return true;
}

void ActorWorld::destroyEntity(entt::entity entity) {
    if (registry_.valid(entity)) {
        if (const auto* actor = registry_.try_get<ActorComponent>(entity)) {
            idToEntity_.erase(actor->id);
        }
        registry_.destroy(entity);
    }
}

entt::entity ActorWorld::getEntity(ActorId id) const {
    const auto it = idToEntity_.find(id);
    return it != idToEntity_.end() ? it->second : entt::null;
}

std::vector<ActorId> ActorWorld::findActorsByName(std::string_view name) const {
    std::vector<ActorId> ids;
    if (name.empty()) {
        return ids;
    }
    const auto view = registry_.view<ActorComponent, NameComponent>();
    for (const auto entity : view) {
        if (view.get<NameComponent>(entity).name == name) {
            ids.push_back(view.get<ActorComponent>(entity).id);
        }
    }
    return ids;
}

void ActorWorld::setName(entt::entity entity, std::string_view name) {
    if (name.empty()) {
        registry_.remove<NameComponent>(entity);
    } else if (const auto* current = registry_.try_get<NameComponent>(entity); !current || current->name != name) {
        registry_.emplace_or_replace<NameComponent>(entity, std::string(name));
    }
}

void ActorWorld::setPlayerMode(entt::entity entity, PlayerMode mode) {
    auto* player = registry_.try_get<PlayerComponent>(entity);
    if (!player || player->mode == mode) {
        return;
    }
    player->mode = mode;
    if (mode == PlayerMode::Spectator) {
        registry_.remove<PhysicsComponent, BoxColliderComponent>(entity);
        registry_.get<MeshComponent>(entity).isVisible = false;
    } else {
        registry_.emplace_or_replace<PhysicsComponent>(entity);
        registry_.emplace_or_replace<BoxColliderComponent>(entity, createPlayerCollider());
        registry_.get<MeshComponent>(entity).isVisible = true;
    }
}
