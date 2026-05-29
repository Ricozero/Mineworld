#include "system.h"

#include <cmath>
#include <cstdlib>

#include "client_world.h"
#include "entity.h"
#include "render_context.h"
#include "server_world.h"

InputSystem::InputSystem(RenderContext* renderContext, uint32_t localSessionId)
    : renderContext_(renderContext), localSessionId_(localSessionId) {
}

void InputSystem::update(ClientWorld& world, float deltaTime) {
    if (!renderContext_) {
        return;
    }

    auto& registry = world.getActorWorld().registry();
    auto view = registry.view<SessionComponent, TransformComponent>();
    for (auto entity : view) {
        const auto& session = registry.get<SessionComponent>(entity);
        if (session.sessionId != localSessionId_) {
            continue;
        }

        auto& transform = registry.get<TransformComponent>(entity);
        // Pass current transform to RenderContext for input processing
        // RenderContext will update position and rotation based on keyboard/mouse
        renderContext_->processInput(deltaTime, transform.position, transform.rotation);
        world.getActorWorld().updateEntityChunk(entity, transform.position);
        inputChanged_ = true;
        break;
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
    // Update random movement for robots
    {
        auto robotView = registry.view<RobotComponent, RandomMovementComponent, PhysicsComponent>();
        for (auto entity : robotView) {
            auto& random = registry.get<RandomMovementComponent>(entity);
            auto& physics = registry.get<PhysicsComponent>(entity);
            const auto& robot = registry.get<RobotComponent>(entity);

            random.changeDirectionTimer -= deltaTime;
            if (random.changeDirectionTimer <= 0.0f) {
                random.changeDirectionTimer = random.changeDirectionInterval;
                const float angle = (static_cast<float>(rand()) / RAND_MAX) * 2.0f * 3.14159265f;
                const float speed = (static_cast<float>(rand()) / RAND_MAX) * robot.moveSpeed;
                random.targetDirection = glm::vec3(std::cos(angle) * speed, 0.0f, std::sin(angle) * speed);
            }

            physics.velocity.x = random.targetDirection.x;
            physics.velocity.z = random.targetDirection.z;
        }
    }

    // Apply physics to all entities with physics
    auto view = registry.view<TransformComponent, PhysicsComponent>();
    for (auto entity : view) {
        auto& transform = registry.get<TransformComponent>(entity);
        auto& physics = registry.get<PhysicsComponent>(entity);
        physics.velocity += physics.acceleration * deltaTime;
        transform.position += physics.velocity * deltaTime;
    }
}

RenderSystem::RenderSystem(RenderContext* renderContext, uint32_t localSessionId)
    : renderContext_(renderContext), localSessionId_(localSessionId) {
}

void RenderSystem::update(ClientWorld& world, float deltaTime) {
    if (!renderContext_) {
        return;
    }

    // Set the camera from the local actor's transform before rendering
    auto& registry = world.getActorWorld().registry();
    auto view = registry.view<SessionComponent, TransformComponent>();
    for (auto entity : view) {
        const auto& session = registry.get<SessionComponent>(entity);
        if (session.sessionId != localSessionId_) {
            continue;
        }
        const auto& transform = registry.get<TransformComponent>(entity);
        renderContext_->setCamera(transform.position, transform.rotation.y, transform.rotation.x);
        break;
    }

    renderContext_->render(world);
}