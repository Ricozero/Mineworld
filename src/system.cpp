#include "system.h"

#include "client_world.h"
#include "entity.h"
#include "render_context.h"
#include "server_world.h"

InputSystem::InputSystem(RenderContext* renderContext) : renderContext_(renderContext) {
}

void InputSystem::update(ClientWorld& world, float deltaTime) {
    if (renderContext_) {
        renderContext_->updateCamera(deltaTime);
    }
    updatePlayerInput(world.getActorWorld().registry(), deltaTime);
}

void InputSystem::updatePlayerInput(entt::registry& registry, float deltaTime) {
    auto view = registry.view<PlayerComponent, TransformComponent, PhysicsComponent>();

    for (auto entity : view) {
    }
}

void PhysicsSystem::update(ServerWorld& world, float deltaTime) {
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

RenderSystem::RenderSystem(RenderContext* renderContext) : renderContext_(renderContext) {
}

void RenderSystem::update(ClientWorld& world, float deltaTime) {
    if (renderContext_) {
        renderContext_->render(world);
        return;
    }
}

void RenderSystem::renderBlock(const glm::ivec3& pos, BlockType type) {
}

void RenderSystem::renderEntity(const std::string& name, const glm::vec3& position, const glm::vec3& color) {
}
