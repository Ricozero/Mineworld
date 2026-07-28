#include "server_system.h"

#include <cmath>
#include <cstdlib>

#include "common_system.h"
#include "entity.h"
#include "profiler.h"
#include "server_world.h"

void PhysicsSystem::update(ServerWorld& world, float deltaTime) {
    MW_PROFILE_SCOPE("Server.Physics");

    auto& registry = world.getActorWorld().registry();
    updateMovement(world, deltaTime);

    auto view = registry.view<TransformComponent, PhysicsComponent>();
    for (auto entity : view) {
        const auto& transform = view.get<TransformComponent>(entity);
        world.getActorWorld().updateEntityChunk(entity, transform.position);
    }
}

void PhysicsSystem::updateMovement(ServerWorld& world, float deltaTime) {
    auto& registry = world.getActorWorld().registry();

    // Update robot AI inputs
    auto robotView = registry.view<RobotComponent, RandomMovementComponent, TransformComponent, ControllerInputComponent>();
    for (auto entity : robotView) {
        auto& random = robotView.get<RandomMovementComponent>(entity);
        auto& input = robotView.get<ControllerInputComponent>(entity);
        auto& transform = robotView.get<TransformComponent>(entity);

        random.changeDirectionTimer -= deltaTime;
        if (random.changeDirectionTimer <= 0.0f) {
            random.changeDirectionTimer = random.changeDirectionInterval;
            const float angle = (static_cast<float>(rand()) / RAND_MAX) * 2.0f * 3.14159265f;
            random.targetDirection = glm::vec3(std::cos(angle), 0.0f, std::sin(angle));
            const bool willJump = (rand() % 3) == 0;
            random.jumpInterval = willJump ? 1.0f + (static_cast<float>(rand()) / RAND_MAX) * 2.0f : 0.0f;
            random.jumpTimer = random.jumpInterval;
        }

        random.jumpTimer -= deltaTime;
        input.move = glm::vec3(0.0f, 0.0f, 1.0f);
        input.jump = random.jumpTimer > 0.0f;
        input.sprint = false;
        if (glm::dot(random.targetDirection, random.targetDirection) > 0.0f) {
            transform.rotation.y = glm::degrees(std::atan2(random.targetDirection.z, random.targetDirection.x));
        }
    }

    // Apply input and step physics for non-player actors
    auto actorView = registry.view<TransformComponent, PhysicsComponent, ControllerInputComponent>(entt::exclude<SessionComponent>);
    for (auto entity : actorView) {
        auto& input = actorView.get<ControllerInputComponent>(entity);
        if (input.jump) {
            common_system::refreshGrounded(world, registry, entity);
        }
        common_system::applyControllerInput(registry, entity, deltaTime, false);
        common_system::simulateActorPhysics(world, registry, entity, deltaTime);
        input.jump = false;
    }
}
