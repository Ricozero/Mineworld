#include "server_actor_manager.h"

#include <cmath>
#include <stdexcept>

#include "profiler.h"

ServerActorManager::ServerActorManager(ActorWorld& world) : world_(world) {}

ActorId ServerActorManager::allocateId() {
    while (nextId_ != 0) {
        const ActorId id = nextId_++;
        if (world_.getEntity(id) == entt::null) {
            return id;
        }
    }
    throw std::overflow_error("Actor ID space exhausted");
}

void ServerActorManager::collectSnapshot() {
    MW_PROFILE_SCOPE("Server.CollectActorStates");
    const auto& registry = world_.registry();
    const auto view = registry.view<ActorComponent, TransformComponent>();
    size_t count = 0;
    for (const auto entity : view) {
        if (count == snapshotStates_.size()) {
            snapshotStates_.emplace_back();
        }
        auto& state = snapshotStates_[count++];
        const auto& transform = view.get<TransformComponent>(entity);
        state.id = view.get<ActorComponent>(entity).id;
        if (const auto* name = registry.try_get<NameComponent>(entity)) {
            state.name = name->name;
        } else {
            state.name.clear();
        }
        state.position = transform.position;
        state.yaw = transform.rotation.y;
        state.pitch = transform.rotation.x;
        const auto* physics = registry.try_get<PhysicsComponent>(entity);
        state.velocity = physics ? physics->velocity : glm::vec3(0.0f);
        const auto* player = registry.try_get<PlayerComponent>(entity);
        const auto* robot = registry.try_get<RobotComponent>(entity);
        state.entityType = EntityType::Actor;
        if (player) {
            state.entityType = EntityType::Player;
        } else if (robot) {
            state.entityType = EntityType::Robot;
        }
        state.playerMode = player ? player->mode : PlayerMode::Survival;
    }
    snapshotStates_.resize(count);
    MW_PROFILE_GAUGE("Server.SnapshotActors", static_cast<double>(count));
}

void ServerActorManager::selectSnapshot(glm::vec3 center, float radius, std::vector<const NetActorState*>& selected) const {
    selected.clear();
    if (!std::isfinite(radius) || radius < 0.0f) {
        return;
    }
    const double radiusSq = static_cast<double>(radius) * radius;
    const glm::dvec3 origin(center);
    for (const auto& state : snapshotStates_) {
        const glm::dvec3 offset = glm::dvec3(state.position) - origin;
        if (glm::dot(offset, offset) <= radiusSq) {
            selected.push_back(&state);
        }
    }
}
