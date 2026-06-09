#include "client_system.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "client_world.h"
#include "config.h"
#include "entity.h"
#include "profiler.h"
#include "render_context.h"
#include "server_system.h"

namespace {

bool isSpectatorPlayer(entt::registry& registry, entt::entity entity) {
    return registry.all_of<PlayerComponent>(entity) &&
           registry.get<PlayerComponent>(entity).mode == PlayerMode::Spectator;
}

bool isSolidBlock(ClientWorld& world, glm::ivec3 worldPos) {
    return world.getBlock(worldPos).type != BlockType::Air;
}

bool findCollisionBoundary(
    ClientWorld& world,
    const TransformComponent& transform,
    const BoxColliderComponent& collider,
    int axis,
    float delta,
    float& boundary) {
    const glm::vec3 halfSize = collider.size * 0.5f;
    const glm::vec3 min = transform.position + collider.offset - halfSize;
    const glm::vec3 max = transform.position + collider.offset + halfSize;
    const glm::ivec3 minBlock{
        static_cast<int>(std::floor(min.x)),
        static_cast<int>(std::floor(min.y)),
        static_cast<int>(std::floor(min.z)),
    };
    const glm::ivec3 maxBlock{
        static_cast<int>(std::floor(max.x - AppConfig::instance().collisionEpsilon)),
        static_cast<int>(std::floor(max.y - AppConfig::instance().collisionEpsilon)),
        static_cast<int>(std::floor(max.z - AppConfig::instance().collisionEpsilon)),
    };

    bool collided = false;
    boundary = delta > 0.0f ? std::numeric_limits<float>::max() : std::numeric_limits<float>::lowest();
    for (int x = minBlock.x; x <= maxBlock.x; ++x) {
        for (int y = minBlock.y; y <= maxBlock.y; ++y) {
            for (int z = minBlock.z; z <= maxBlock.z; ++z) {
                const glm::ivec3 blockPos(x, y, z);
                if (!isSolidBlock(world, blockPos)) {
                    continue;
                }

                collided = true;
                const float blockBoundary = delta > 0.0f
                                                ? static_cast<float>(blockPos[axis])
                                                : static_cast<float>(blockPos[axis] + 1);
                boundary = delta > 0.0f ? std::min(boundary, blockBoundary) : std::max(boundary, blockBoundary);
            }
        }
    }

    return collided;
}

bool hasCollision(ClientWorld& world, const TransformComponent& transform, const BoxColliderComponent& collider) {
    float boundary = 0.0f;
    return findCollisionBoundary(world, transform, collider, 1, -1.0f, boundary);
}

void refreshGrounded(ClientWorld& world, entt::registry& registry, entt::entity entity) {
    if (!registry.all_of<TransformComponent, PhysicsComponent, BoxColliderComponent>(entity)) {
        return;
    }
    auto& physics = registry.get<PhysicsComponent>(entity);
    if (physics.isGrounded) {
        return;
    }

    TransformComponent probe = registry.get<TransformComponent>(entity);
    probe.position.y -= AppConfig::instance().groundProbeDistance;
    physics.isGrounded = hasCollision(world, probe, registry.get<BoxColliderComponent>(entity));
}

void moveWithCollision(ClientWorld& world, entt::registry& registry, entt::entity entity, float deltaTime) {
    auto& transform = registry.get<TransformComponent>(entity);
    auto& physics = registry.get<PhysicsComponent>(entity);
    const auto& collider = registry.get<BoxColliderComponent>(entity);

    physics.isGrounded = false;
    const glm::vec3 movement = physics.velocity * deltaTime;
    const glm::vec3 halfSize = collider.size * 0.5f;

    for (int axis = 0; axis < 3; ++axis) {
        const float delta = movement[axis];
        if (std::abs(delta) <= std::numeric_limits<float>::epsilon()) {
            continue;
        }

        transform.position[axis] += delta;
        float boundary = 0.0f;
        if (!findCollisionBoundary(world, transform, collider, axis, delta, boundary)) {
            continue;
        }

        if (delta > 0.0f) {
            transform.position[axis] = boundary - collider.offset[axis] - halfSize[axis] - AppConfig::instance().collisionEpsilon;
        } else {
            transform.position[axis] = boundary - collider.offset[axis] + halfSize[axis] + AppConfig::instance().collisionEpsilon;
            if (axis == 1) {
                physics.isGrounded = true;
            }
        }
        physics.velocity[axis] = 0.0f;
    }

    if (!physics.isGrounded) {
        TransformComponent probe = transform;
        probe.position.y -= AppConfig::instance().groundProbeDistance;
        physics.isGrounded = hasCollision(world, probe, collider);
    }
    physics.acceleration = glm::vec3(0.0f);
}

}  // namespace

void simulateClientActor(ClientWorld& world, entt::registry& registry, entt::entity entity, float deltaTime) {
    if (isSpectatorPlayer(registry, entity) || !registry.all_of<TransformComponent, PhysicsComponent>(entity)) {
        return;
    }

    auto& transform = registry.get<TransformComponent>(entity);
    auto& physics = registry.get<PhysicsComponent>(entity);
    if (physics.jumpImpulseTime > 0.0f) {
        const float impulseDelta = std::min(deltaTime, physics.jumpImpulseTime);
        physics.velocity.y += AppConfig::instance().jumpAcceleration * impulseDelta;
        physics.jumpImpulseTime -= impulseDelta;
    }
    if (physics.useGravity && !physics.isGrounded) {
        physics.velocity.y -= AppConfig::instance().gravity * deltaTime;
        physics.velocity.y = std::max(physics.velocity.y, -AppConfig::instance().maxFallSpeed);
    }

    physics.velocity += physics.acceleration * deltaTime;
    if (registry.all_of<BoxColliderComponent>(entity)) {
        moveWithCollision(world, registry, entity, deltaTime);
    } else {
        transform.position += physics.velocity * deltaTime;
        physics.acceleration = glm::vec3(0.0f);
    }
}

void applyClientPredictedInput(ClientWorld& world, entt::registry& registry, entt::entity entity, const PredictedInput& predictedInput) {
    if (!registry.all_of<TransformComponent, ControllerInputComponent, PlayerComponent>(entity)) {
        return;
    }

    auto& player = registry.get<PlayerComponent>(entity);
    player.mode = predictedInput.playerMode;
    world.getActorWorld().setPlayerMode(entity, player.mode);

    auto& transform = registry.get<TransformComponent>(entity);
    transform.rotation = predictedInput.rotation;

    auto& controllerInput = registry.get<ControllerInputComponent>(entity);
    controllerInput = predictedInput.input;
    if (controllerInput.jump) {
        refreshGrounded(world, registry, entity);
    }
    applyControllerInput(registry, entity, predictedInput.deltaTime, false);
    simulateClientActor(world, registry, entity, predictedInput.deltaTime);
}

InputSystem::InputSystem(RenderContext* renderContext, uint32_t localSessionId)
    : renderContext_(renderContext), localSessionId_(localSessionId) {
}

void InputSystem::update(ClientWorld& world, float deltaTime) {
    MW_PROFILE_SCOPE("System.Input");

    if (!renderContext_) {
        return;
    }

    auto& registry = world.getActorWorld().registry();
    auto view = registry.view<SessionComponent, TransformComponent, PlayerComponent, ControllerInputComponent>();
    for (auto entity : view) {
        const auto& session = registry.get<SessionComponent>(entity);
        if (session.sessionId != localSessionId_) {
            continue;
        }

        auto& transform = registry.get<TransformComponent>(entity);
        auto& player = registry.get<PlayerComponent>(entity);
        auto& input = registry.get<ControllerInputComponent>(entity);
        input.deltaTime = deltaTime;
        const glm::vec3 previousRotation = transform.rotation;
        const PlayerMode previousMode = player.mode;
        const ControllerInputComponent previousInput = input;
        renderContext_->processInput(deltaTime, transform.rotation, player, input);
        const bool jumpForServer = input.jump;
        applyClientPredictedInput(world, registry, entity, PredictedInput{input, player.mode, transform.rotation, deltaTime});
        input.jump = jumpForServer;
        world.getActorWorld().updateEntityChunk(entity, transform.position);
        inputChanged_ = previousRotation != transform.rotation || previousMode != player.mode ||
                        previousInput.move != input.move || previousInput.jump != input.jump ||
                        previousInput.sprint != input.sprint;
        pendingInput_ = true;
        break;
    }
}

RenderSystem::RenderSystem(RenderContext* renderContext, uint32_t localSessionId)
    : renderContext_(renderContext), localSessionId_(localSessionId) {
}

void RenderSystem::update(ClientWorld& world, float deltaTime) {
    MW_PROFILE_SCOPE("System.Render");

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
        const auto& transform = registry.get<TransformComponent>(entity);
        const auto& player = registry.get<PlayerComponent>(entity);
        renderContext_->setCamera(transform.position, transform.rotation.y, transform.rotation.x, player.mode, localSessionId_);
        break;
    }

    renderContext_->render(world);
}
