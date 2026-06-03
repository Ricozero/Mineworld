#include "system.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "client_world.h"
#include "entity.h"
#include "profiler.h"
#include "render_context.h"
#include "server_world.h"

namespace {

constexpr float kGravity = 9.8f;
constexpr float kCollisionEpsilon = 0.001f;

bool isSpectatorPlayer(entt::registry& registry, entt::entity entity) {
    return registry.all_of<PlayerComponent>(entity) &&
           registry.get<PlayerComponent>(entity).mode == PlayerMode::Spectator;
}

bool isSolidBlock(ServerWorld& world, glm::ivec3 worldPos) {
    return world.getBlock(worldPos).type != BlockType::Air;
}

bool findCollisionBoundary(
    ServerWorld& world,
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
        static_cast<int>(std::floor(max.x - kCollisionEpsilon)),
        static_cast<int>(std::floor(max.y - kCollisionEpsilon)),
        static_cast<int>(std::floor(max.z - kCollisionEpsilon)),
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

bool hasCollision(ServerWorld& world, const TransformComponent& transform, const BoxColliderComponent& collider) {
    float boundary = 0.0f;
    return findCollisionBoundary(world, transform, collider, 1, -1.0f, boundary);
}

}  // namespace

InputSystem::InputSystem(RenderContext* renderContext, uint32_t localSessionId)
    : renderContext_(renderContext), localSessionId_(localSessionId) {
}

void InputSystem::update(ClientWorld& world, float deltaTime) {
    MW_PROFILE_SCOPE("System.Input");

    if (!renderContext_) {
        return;
    }

    auto& registry = world.getActorWorld().registry();
    auto view = registry.view<SessionComponent, TransformComponent, PlayerComponent>();
    for (auto entity : view) {
        const auto& session = registry.get<SessionComponent>(entity);
        if (session.sessionId != localSessionId_) {
            continue;
        }

        auto& transform = registry.get<TransformComponent>(entity);
        auto& player = registry.get<PlayerComponent>(entity);
        // Pass current transform to RenderContext for input processing
        // RenderContext will update position and rotation based on keyboard/mouse
        renderContext_->processInput(deltaTime, transform.position, transform.rotation, player);
        world.getActorWorld().setPlayerMode(entity, player.mode);
        world.getActorWorld().updateEntityChunk(entity, transform.position);
        inputChanged_ = true;
        break;
    }
}

void PhysicsSystem::update(ServerWorld& world, float deltaTime) {
    MW_PROFILE_SCOPE("System.Physics");

    auto& registry = world.getActorWorld().registry();
    applyGravity(registry, deltaTime);
    updateMovement(world, deltaTime);

    auto view = registry.view<TransformComponent, PhysicsComponent>();
    for (auto entity : view) {
        auto& transform = registry.get<TransformComponent>(entity);
        world.getActorWorld().updateEntityChunk(entity, transform.position);
    }
}

void PhysicsSystem::applyGravity(entt::registry& registry, float deltaTime) {
    MW_PROFILE_SCOPE("System.Physics.Gravity");

    auto view = registry.view<PhysicsComponent, TransformComponent>();

    for (auto entity : view) {
        if (isSpectatorPlayer(registry, entity)) {
            continue;
        }
        auto& physics = registry.get<PhysicsComponent>(entity);
        if (physics.useGravity && !physics.isGrounded) {
            physics.velocity.y -= kGravity * deltaTime;
        }
    }
}

void PhysicsSystem::updateMovement(ServerWorld& world, float deltaTime) {
    MW_PROFILE_SCOPE("System.Physics.Movement");

    auto& registry = world.getActorWorld().registry();

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
        if (isSpectatorPlayer(registry, entity)) {
            continue;
        }
        auto& transform = registry.get<TransformComponent>(entity);
        auto& physics = registry.get<PhysicsComponent>(entity);
        physics.velocity += physics.acceleration * deltaTime;
        if (registry.all_of<BoxColliderComponent>(entity)) {
            moveWithCollision(world, entity, deltaTime);
        } else {
            transform.position += physics.velocity * deltaTime;
            physics.acceleration = glm::vec3(0.0f);
        }
    }
}

void PhysicsSystem::moveWithCollision(ServerWorld& world, entt::entity entity, float deltaTime) {
    MW_PROFILE_SCOPE("System.Physics.Collision");

    auto& registry = world.getActorWorld().registry();
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
            transform.position[axis] = boundary - collider.offset[axis] - halfSize[axis] - kCollisionEpsilon;
        } else {
            transform.position[axis] = boundary - collider.offset[axis] + halfSize[axis] + kCollisionEpsilon;
            if (axis == 1) {
                physics.isGrounded = true;
            }
        }
        physics.velocity[axis] = 0.0f;
    }

    if (!physics.isGrounded) {
        TransformComponent probe = transform;
        probe.position.y -= kCollisionEpsilon * 2.0f;
        physics.isGrounded = hasCollision(world, probe, collider);
    }
    physics.acceleration = glm::vec3(0.0f);
}

RenderSystem::RenderSystem(RenderContext* renderContext, uint32_t localSessionId)
    : renderContext_(renderContext), localSessionId_(localSessionId) {
}

void RenderSystem::update(ClientWorld& world, float deltaTime) {
    MW_PROFILE_SCOPE("System.Render");

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
