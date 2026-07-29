#pragma once

#include <algorithm>
#include <cmath>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <limits>

#include "block.h"
#include "chunk.h"
#include "config.h"
#include "entity.h"

namespace common_system {

inline constexpr float kCollisionEpsilon = 0.001f;
inline constexpr float kGroundProbeDistance = 0.05f;

template <typename World>
class BaseSystem {
public:
    virtual ~BaseSystem() = default;
    virtual void update(World& world, float deltaTime) = 0;
};

inline bool isSpectatorPlayer(entt::registry& registry, entt::entity entity) {
    return registry.all_of<PlayerComponent>(entity) && registry.get<PlayerComponent>(entity).mode == PlayerMode::Spectator;
}

template <typename World>
BlockQueryResult queryCollisionBlock(World& world, glm::ivec3 worldPos) {
    return world.queryBlock(worldPos);
}

template <typename World>
BlockQueryResult findCollisionBoundary(World& world, const TransformComponent& transform, const BoxColliderComponent& collider, int axis, float delta, float& boundary) {
    const glm::vec3 halfSize = collider.size * 0.5f;
    const glm::vec3 currentMin = transform.position + collider.offset - halfSize;
    const glm::vec3 currentMax = transform.position + collider.offset + halfSize;
    glm::vec3 previousMin = currentMin;
    glm::vec3 previousMax = currentMax;
    previousMin[axis] -= delta;
    previousMax[axis] -= delta;
    const glm::vec3 sweptMin = glm::min(previousMin, currentMin);
    const glm::vec3 sweptMax = glm::max(previousMax, currentMax);
    const glm::ivec3 minBlock{
        static_cast<int>(std::floor(sweptMin.x)),
        static_cast<int>(std::floor(sweptMin.y)),
        static_cast<int>(std::floor(sweptMin.z)),
    };
    const glm::ivec3 maxBlock{
        static_cast<int>(std::floor(sweptMax.x - kCollisionEpsilon)),
        static_cast<int>(std::floor(sweptMax.y - kCollisionEpsilon)),
        static_cast<int>(std::floor(sweptMax.z - kCollisionEpsilon)),
    };

    bool collided = false;
    boundary = delta > 0.0f ? std::numeric_limits<float>::max() : std::numeric_limits<float>::lowest();
    for (int x = minBlock.x; x <= maxBlock.x; ++x) {
        for (int y = minBlock.y; y <= maxBlock.y; ++y) {
            for (int z = minBlock.z; z <= maxBlock.z; ++z) {
                const glm::ivec3 blockPos(x, y, z);
                const BlockQueryResult blockResult = queryCollisionBlock(world, blockPos);
                if (blockResult == BlockQueryResult::Unknown) {
                    return BlockQueryResult::Unknown;
                }
                if (blockResult == BlockQueryResult::Empty) {
                    continue;
                }

                const float blockBoundary = delta > 0.0f
                                                ? static_cast<float>(blockPos[axis])
                                                : static_cast<float>(blockPos[axis] + 1);
                if (delta > 0.0f) {
                    if (blockBoundary < previousMax[axis] - kCollisionEpsilon ||
                        blockBoundary > currentMax[axis]) {
                        continue;
                    }
                } else if (blockBoundary > previousMin[axis] + kCollisionEpsilon ||
                           blockBoundary < currentMin[axis]) {
                    continue;
                }

                collided = true;
                boundary = delta > 0.0f ? std::min(boundary, blockBoundary) : std::max(boundary, blockBoundary);
            }
        }
    }

    return collided ? BlockQueryResult::Solid : BlockQueryResult::Empty;
}

template <typename World>
BlockQueryResult testCollision(World& world, const TransformComponent& transform, const BoxColliderComponent& collider) {
    const glm::vec3 halfSize = collider.size * 0.5f;
    const glm::vec3 min = transform.position + collider.offset - halfSize;
    const glm::vec3 max = transform.position + collider.offset + halfSize;
    const glm::ivec3 minBlock = glm::ivec3(glm::floor(min));
    const glm::ivec3 maxBlock = glm::ivec3(glm::floor(max - glm::vec3(kCollisionEpsilon)));

    bool collided = false;
    for (int x = minBlock.x; x <= maxBlock.x; ++x) {
        for (int y = minBlock.y; y <= maxBlock.y; ++y) {
            for (int z = minBlock.z; z <= maxBlock.z; ++z) {
                const BlockQueryResult result = queryCollisionBlock(world, {x, y, z});
                if (result == BlockQueryResult::Unknown) {
                    return BlockQueryResult::Unknown;
                }
                if (result == BlockQueryResult::Solid) {
                    collided = true;
                }
            }
        }
    }
    return collided ? BlockQueryResult::Solid : BlockQueryResult::Empty;
}

template <typename World>
bool areChunksLoadedInBounds(World& world, const glm::vec3& min, const glm::vec3& max) {
    const glm::ivec3 minBlock = glm::ivec3(glm::floor(min));
    const glm::ivec3 maxBlock = glm::ivec3(glm::floor(max - glm::vec3(kCollisionEpsilon)));
    const glm::ivec3 minChunk = Chunk::worldToChunk(minBlock);
    const glm::ivec3 maxChunk = Chunk::worldToChunk(maxBlock);

    for (int x = minChunk.x; x <= maxChunk.x; ++x) {
        for (int y = minChunk.y; y <= maxChunk.y; ++y) {
            for (int z = minChunk.z; z <= maxChunk.z; ++z) {
                if (!world.getVoxelWorld().isChunkLoaded({x, y, z})) {
                    return false;
                }
            }
        }
    }
    return true;
}

template <typename World>
bool areSweptBoxChunksLoaded(World& world, const TransformComponent& transform, const BoxColliderComponent& collider, const glm::vec3& movement) {
    const glm::vec3 halfSize = collider.size * 0.5f;
    const glm::vec3 currentMin = transform.position + collider.offset - halfSize;
    const glm::vec3 currentMax = transform.position + collider.offset + halfSize;
    glm::vec3 sweptMin = glm::min(currentMin, currentMin + movement);
    const glm::vec3 sweptMax = glm::max(currentMax, currentMax + movement);
    sweptMin.y -= kGroundProbeDistance;
    return areChunksLoadedInBounds(world, sweptMin, sweptMax);
}

template <typename World>
void refreshGrounded(World& world, entt::registry& registry, entt::entity entity) {
    if (!registry.all_of<TransformComponent, PhysicsComponent, BoxColliderComponent>(entity)) {
        return;
    }
    auto& physics = registry.get<PhysicsComponent>(entity);
    if (physics.isGrounded) {
        return;
    }

    TransformComponent probe = registry.get<TransformComponent>(entity);
    probe.position.y -= kGroundProbeDistance;
    const BlockQueryResult result = testCollision(world, probe, registry.get<BoxColliderComponent>(entity));
    if (result != BlockQueryResult::Unknown) {
        physics.isGrounded = result == BlockQueryResult::Solid;
    }
}

template <typename World>
bool moveWithCollision(World& world, entt::registry& registry, entt::entity entity, float deltaTime) {
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
        const BlockQueryResult result = findCollisionBoundary(world, transform, collider, axis, delta, boundary);
        if (result == BlockQueryResult::Unknown) {
            return false;
        }
        if (result == BlockQueryResult::Empty) {
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
        probe.position.y -= kGroundProbeDistance;
        const BlockQueryResult result = testCollision(world, probe, collider);
        if (result == BlockQueryResult::Unknown) {
            return false;
        }
        physics.isGrounded = result == BlockQueryResult::Solid;
    }
    physics.acceleration = glm::vec3(0.0f);
    return true;
}

template <typename World>
void simulateActorPhysics(World& world, entt::registry& registry, entt::entity entity, float deltaTime) {
    if (isSpectatorPlayer(registry, entity) || !registry.all_of<TransformComponent, PhysicsComponent>(entity)) {
        return;
    }

    auto& transform = registry.get<TransformComponent>(entity);
    auto& physics = registry.get<PhysicsComponent>(entity);

    const glm::vec3 initialPosition = transform.position;
    const bool initialGrounded = physics.isGrounded;
    if (physics.useGravity && !physics.isGrounded) {
        physics.velocity.y -= AppConfig::instance().gravity * deltaTime;
        physics.velocity.y = std::max(physics.velocity.y, -AppConfig::instance().maxFallSpeed);
    }

    physics.velocity += physics.acceleration * deltaTime;
    const glm::vec3 movement = physics.velocity * deltaTime;
    if (registry.all_of<BoxColliderComponent>(entity)) {
        const auto& collider = registry.get<BoxColliderComponent>(entity);
        if (!areSweptBoxChunksLoaded(world, transform, collider, movement) ||
            !moveWithCollision(world, registry, entity, deltaTime)) {
            transform.position = initialPosition;
            physics.velocity = glm::vec3(0.0f);
            physics.acceleration = glm::vec3(0.0f);
            physics.isGrounded = initialGrounded;
        }
    } else {
        transform.position += movement;
        physics.acceleration = glm::vec3(0.0f);
    }
}

inline glm::vec3 yawForward(float yawDegrees) {
    const float yaw = glm::radians(yawDegrees);
    return glm::normalize(glm::vec3(std::cos(yaw), 0.0f, std::sin(yaw)));
}

inline glm::vec3 yawRight(float yawDegrees) {
    return glm::normalize(glm::cross(yawForward(yawDegrees), glm::vec3(0.0f, 1.0f, 0.0f)));
}

inline glm::vec3 lookForward(float yawDegrees, float pitchDegrees) {
    const float yaw = glm::radians(yawDegrees);
    const float pitch = glm::radians(pitchDegrees);
    return glm::normalize(glm::vec3(
        std::cos(yaw) * std::cos(pitch),
        std::sin(pitch),
        std::sin(yaw) * std::cos(pitch)));
}

inline glm::vec3 lookRight(float yawDegrees, float pitchDegrees) {
    return glm::normalize(glm::cross(lookForward(yawDegrees, pitchDegrees), glm::vec3(0.0f, 1.0f, 0.0f)));
}

inline float movementSpeed(entt::registry& registry, entt::entity entity, const ControllerInputComponent& input) {
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

inline void applyControllerInput(entt::registry& registry, entt::entity entity, float deltaTime, bool consumeJump) {
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
        const float airborneMultiplier = physics.isGrounded ? 1.0f : AppConfig::instance().airborneSpeedMultiplier;
        physics.velocity.x = move.x * speed * airborneMultiplier;
        physics.velocity.z = move.z * speed * airborneMultiplier;
        if (input.jump && physics.isGrounded) {
            physics.velocity.y = AppConfig::instance().jumpSpeed;
            physics.isGrounded = false;
        }
    }

    if (consumeJump) {
        input.jump = false;
    }
}

}  // namespace common_system
