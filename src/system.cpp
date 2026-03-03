#include "system.h"

#include <spdlog/spdlog.h>

#include "entity.h"
#include "world.h"

void InputSystem::update(World& world, float deltaTime) {
    updatePlayerInput(world.getActorWorld().registry(), deltaTime);
}

void InputSystem::updatePlayerInput(entt::registry& registry, float deltaTime) {
    auto view = registry.view<PlayerComponent, TransformComponent, PhysicsComponent>();

    for (auto entity : view) {
    }
}

void PhysicsSystem::update(World& world, float deltaTime) {
    auto& registry = world.getActorWorld().registry();
    applyGravity(registry, deltaTime);
    updateMovement(registry, deltaTime);

    auto view = registry.view<TransformComponent, PhysicsComponent>();
    for (auto entity : view) {
        auto& transform = registry.get<TransformComponent>(entity);
        world.getActorWorld().updateEntityChunk(entity, transform.position);
    }
}

void PhysicsSystem::applyGravity(entt::registry& registry, float deltaTime) {
    auto view = registry.view<PhysicsComponent, TransformComponent>();
    constexpr float gravity = 9.8f;

    for (auto entity : view) {
        auto& physics = registry.get<PhysicsComponent>(entity);
        auto& acceleration = physics.acceleration;
        if (physics.useGravity && !physics.isGrounded) {
            acceleration.y -= gravity * deltaTime;
        }
    }
}

void PhysicsSystem::updateMovement(entt::registry& registry, float deltaTime) {
    auto view = registry.view<TransformComponent, PhysicsComponent>();

    for (auto entity : view) {
        auto& transform = registry.get<TransformComponent>(entity);
        auto& physics = registry.get<PhysicsComponent>(entity);
        physics.velocity += physics.acceleration * deltaTime;
        transform.position += physics.velocity * deltaTime;
    }
}

void RenderSystem::update(World& world, float deltaTime) {
    static float totalTime = 0.0f;
    totalTime += deltaTime;
    if (int(totalTime + deltaTime) <= int(totalTime)) return;
    spdlog::info("Rendering world at time {:.2f}s", totalTime);

    auto& registry = world.getActorWorld().registry();
    auto playerView = registry.view<PlayerComponent, TransformComponent>();
    for (auto entity : playerView) {
        auto& player = registry.get<PlayerComponent>(entity);
        auto& transform = registry.get<TransformComponent>(entity);
        auto& name = registry.get<NameComponent>(entity);
        spdlog::info("Player {} at ({:.2f}, {:.2f}, {:.2f})", name.name, transform.position.x, transform.position.y, transform.position.z);
    }
}

void RenderSystem::renderBlock(const glm::ivec3& pos, BlockType type) {
}

void RenderSystem::renderEntity(const std::string& name, const glm::vec3& position, const glm::vec3& color) {
}
