#include "client_system.h"

#include <cmath>

#include "chunk.h"
#include "client_world.h"
#include "common_system.h"
#include "entity.h"
#include "profiler.h"
#include "render_context.h"

namespace {

bool isInLoadedChunk(ClientWorld& world, entt::registry& registry, entt::entity entity) {
    if (!registry.all_of<TransformComponent>(entity)) {
        return false;
    }
    const glm::vec3& pos = registry.get<TransformComponent>(entity).position;
    const glm::ivec3 chunk = Chunk::worldToChunk(glm::ivec3(
        static_cast<int>(std::floor(pos.x)),
        static_cast<int>(std::floor(pos.y)),
        static_cast<int>(std::floor(pos.z))));
    return world.getVoxelWorld().isChunkLoaded(chunk);
}

}  // namespace

InputSystem::InputSystem(RenderContext* renderContext, uint32_t localSessionId)
    : renderContext_(renderContext), localSessionId_(localSessionId) {
}

void InputSystem::update(ClientWorld& world, float deltaTime) {
    MW_PROFILE_SCOPE("Client.Input");

    if (!renderContext_) {
        return;
    }

    auto& registry = world.getActorWorld().registry();
    auto view = registry.view<SessionComponent, TransformComponent, PlayerComponent, ControllerInputComponent, PhysicsComponent>();
    for (auto entity : view) {
        const auto& session = registry.get<SessionComponent>(entity);
        if (session.sessionId != localSessionId_) {
            continue;
        }

        auto& transform = registry.get<TransformComponent>(entity);
        auto& player = registry.get<PlayerComponent>(entity);
        auto& input = registry.get<ControllerInputComponent>(entity);
        auto& physics = registry.get<PhysicsComponent>(entity);
        input.deltaTime = deltaTime;
        const glm::vec3 previousRotation = transform.rotation;
        const PlayerMode previousMode = player.mode;
        const ControllerInputComponent previousInput = input;
        renderContext_->processInput(deltaTime, transform.rotation, player, input);

        if (!isInLoadedChunk(world, registry, entity)) {
            physics.velocity = glm::vec3(0.0f);
            continue;
        }

        if (input.jump) {
            common_system::refreshGrounded(world, registry, entity);
        }
        common_system::applyControllerInput(registry, entity, deltaTime, false);
        common_system::simulateActorPhysics(world, registry, entity, deltaTime);

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
    MW_PROFILE_SCOPE("Client.Render");

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
