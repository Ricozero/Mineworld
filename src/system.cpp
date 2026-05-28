#include "system.h"

#include <cmath>
#include <cstdlib>

#include "client_world.h"
#include "entity.h"
#include "render_context.h"
#include "server_world.h"

InputSystem::InputSystem(RenderContext* renderContext, const std::string& spectatorName)
    : renderContext_(renderContext), spectatorName_(spectatorName) {
}

void InputSystem::update(ClientWorld& world, float deltaTime) {
    if (renderContext_) {
        renderContext_->updateCamera(deltaTime);

        // Sync camera position to client spectator entity
        if (!spectatorName_.empty()) {
            entt::entity spectator = world.getEntityByName(spectatorName_);
            if (spectator != entt::null) {
                auto& registry = world.getActorWorld().registry();
                if (registry.all_of<SpectatorComponent, TransformComponent>(spectator)) {
                    auto& transform = registry.get<TransformComponent>(spectator);
                    transform.position = renderContext_->getCameraPosition();
                }
            }
        }
    }
    updatePlayerInput(world.getActorWorld().registry(), deltaTime);
}

void InputSystem::updatePlayerInput(entt::registry& registry, float deltaTime) {
    auto view = registry.view<PlayerComponent, TransformComponent, PhysicsComponent>();

    for (auto entity : view) {
        (void)entity;
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
    // Update random movement for players
    {
        auto playerView = registry.view<PlayerComponent, RandomMovementComponent, PhysicsComponent>();
        for (auto entity : playerView) {
            auto& random = registry.get<RandomMovementComponent>(entity);
            auto& physics = registry.get<PhysicsComponent>(entity);
            const auto& player = registry.get<PlayerComponent>(entity);

            random.changeDirectionTimer -= deltaTime;
            if (random.changeDirectionTimer <= 0.0f) {
                random.changeDirectionTimer = random.changeDirectionInterval;
                // Generate random direction in XZ plane (no vertical movement)
                const float angle = (static_cast<float>(rand()) / RAND_MAX) * 2.0f * 3.14159265f;
                const float speed = (static_cast<float>(rand()) / RAND_MAX) * player.moveSpeed;
                random.targetDirection = glm::vec3(std::cos(angle) * speed, 0.0f, std::sin(angle) * speed);
            }

            physics.velocity.x = random.targetDirection.x;
            physics.velocity.z = random.targetDirection.z;
        }
    }

    // Apply physics to all entities
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