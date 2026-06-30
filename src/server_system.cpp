#include "server_system.h"

#include <cmath>
#include <cstdlib>

#include "common_system.h"
#include "config.h"
#include "entity.h"
#include "profiler.h"
#include "server_world.h"

namespace {

glm::vec3 yawForward(float yawDegrees) {
    const float yaw = glm::radians(yawDegrees);
    return glm::normalize(glm::vec3(std::cos(yaw), 0.0f, std::sin(yaw)));
}

glm::vec3 yawRight(float yawDegrees) {
    return glm::normalize(glm::cross(yawForward(yawDegrees), glm::vec3(0.0f, 1.0f, 0.0f)));
}

glm::vec3 lookForward(float yawDegrees, float pitchDegrees) {
    const float yaw = glm::radians(yawDegrees);
    const float pitch = glm::radians(pitchDegrees);
    return glm::normalize(glm::vec3(
        std::cos(yaw) * std::cos(pitch),
        std::sin(pitch),
        std::sin(yaw) * std::cos(pitch)));
}

glm::vec3 lookRight(float yawDegrees, float pitchDegrees) {
    return glm::normalize(glm::cross(lookForward(yawDegrees, pitchDegrees), glm::vec3(0.0f, 1.0f, 0.0f)));
}

float movementSpeed(entt::registry& registry, entt::entity entity, const ControllerInputComponent& input) {
    if (registry.all_of<PlayerComponent>(entity)) {
        const auto& player = registry.get<PlayerComponent>(entity);
        const bool spectator = player.mode == PlayerMode::Spectator;
        const float baseSpeed = spectator ? player.spectatorMoveSpeed : player.survivalMoveSpeed;
        const float multiplier = spectator ? AppConfig::instance().spectatorSprintMultiplier : AppConfig::instance().survivalSprintMultiplier;
        return input.sprint ? baseSpeed * multiplier : baseSpeed;
    }
    if (registry.all_of<RobotComponent>(entity)) {
        return registry.get<RobotComponent>(entity).moveSpeed;
    }
    return 0.0f;
}

}  // namespace

void applyControllerInput(entt::registry& registry, entt::entity entity, float deltaTime, bool consumeJump) {
    if (!registry.all_of<TransformComponent, ControllerInputComponent>(entity)) {
        return;
    }

    auto& transform = registry.get<TransformComponent>(entity);
    auto& input = registry.get<ControllerInputComponent>(entity);
    const float speed = movementSpeed(registry, entity, input);
    const bool spectator = common_system::isSpectatorPlayer(registry, entity);

    if (spectator) {
        glm::vec3 move = lookForward(transform.rotation.y, transform.rotation.x) * input.move.z +
                         lookRight(transform.rotation.y, transform.rotation.x) * input.move.x +
                         glm::vec3(0.0f, input.move.y, 0.0f);
        if (glm::dot(move, move) > 1.0f) {
            move = glm::normalize(move);
        }
        transform.position += move * speed * deltaTime;
    } else if (registry.all_of<PhysicsComponent>(entity)) {
        glm::vec3 move = yawForward(transform.rotation.y) * input.move.z + yawRight(transform.rotation.y) * input.move.x;
        if (glm::dot(move, move) > 1.0f) {
            move = glm::normalize(move);
        }
        auto& physics = registry.get<PhysicsComponent>(entity);
        physics.velocity.x = move.x * speed;
        physics.velocity.z = move.z * speed;
        if (input.jump && physics.isGrounded) {
            physics.velocity.y = AppConfig::instance().jumpSpeed;
            physics.isGrounded = false;
        }
    }

    if (consumeJump) {
        input.jump = false;
    }
}

void simulateServerActor(ServerWorld& world, entt::registry& registry, entt::entity entity, float deltaTime) {
    common_system::simulateActorPhysics(world, registry, entity, deltaTime);
    if (registry.all_of<TransformComponent>(entity)) {
        world.getActorWorld().updateEntityChunk(entity, registry.get<TransformComponent>(entity).position);
    }
}

void PhysicsSystem::update(ServerWorld& world, float deltaTime) {
    MW_PROFILE_SCOPE("Server.Physics");

    auto& registry = world.getActorWorld().registry();
    updateMovement(world, deltaTime);

    auto view = registry.view<TransformComponent, PhysicsComponent>();
    for (auto entity : view) {
        auto& transform = registry.get<TransformComponent>(entity);
        world.getActorWorld().updateEntityChunk(entity, transform.position);
    }
}

void PhysicsSystem::updateMovement(ServerWorld& world, float deltaTime) {
    MW_PROFILE_SCOPE("Server.Physics.Movement");

    auto& registry = world.getActorWorld().registry();

    // Update robot AI inputs
    auto robotView = registry.view<RobotComponent, RandomMovementComponent, TransformComponent, ControllerInputComponent>();
    for (auto entity : robotView) {
        auto& random = registry.get<RandomMovementComponent>(entity);
        auto& input = registry.get<ControllerInputComponent>(entity);
        auto& transform = registry.get<TransformComponent>(entity);

        random.changeDirectionTimer -= deltaTime;
        if (random.changeDirectionTimer <= 0.0f) {
            random.changeDirectionTimer = random.changeDirectionInterval;
            const float angle = (static_cast<float>(rand()) / RAND_MAX) * 2.0f * 3.14159265f;
            random.targetDirection = glm::vec3(std::cos(angle), 0.0f, std::sin(angle));
        }

        input.move = glm::vec3(0.0f, 0.0f, 1.0f);
        input.jump = false;
        input.sprint = false;
        if (glm::dot(random.targetDirection, random.targetDirection) > 0.0f) {
            transform.rotation.y = glm::degrees(std::atan2(random.targetDirection.z, random.targetDirection.x));
        }
    }

    // Apply input and step physics
    auto actorView = registry.view<TransformComponent, PhysicsComponent, ControllerInputComponent>();
    for (auto entity : actorView) {
        if (common_system::isSpectatorPlayer(registry, entity)) {
            continue;
        }
        auto& input = registry.get<ControllerInputComponent>(entity);
        const bool consumeJump = registry.all_of<SessionComponent>(entity);
        if (input.jump) {
            common_system::refreshGrounded(world, registry, entity);
        }
        applyControllerInput(registry, entity, deltaTime, consumeJump);
        simulateServerActor(world, registry, entity, deltaTime);
        if (!consumeJump) {
            input.jump = false;
        }
    }
}
