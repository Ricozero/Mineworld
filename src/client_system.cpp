#include "client_system.h"

#include "actor_simulation.h"
#include "actor_world.h"
#include "client_chunk_manager.h"
#include "entity.h"
#include "profiler.h"
#include "render_context.h"

InputSystem::InputSystem(RenderContext* renderContext, uint32_t localSessionId)
    : renderContext_(renderContext), localSessionId_(localSessionId) {
}

void InputSystem::update(VoxelWorld& voxelWorld, ActorWorld& actorWorld, float deltaTime) {
    MW_PROFILE_SCOPE("Client.Input");

    if (!renderContext_) {
        return;
    }

    auto& registry = actorWorld.registry();
    auto view = registry.view<SessionComponent, TransformComponent, PlayerComponent, ControllerInputComponent>();
    for (auto entity : view) {
        const auto& session = view.get<SessionComponent>(entity);
        if (session.sessionId != localSessionId_) {
            continue;
        }

        auto& transform = view.get<TransformComponent>(entity);
        auto& player = view.get<PlayerComponent>(entity);
        auto& input = view.get<ControllerInputComponent>(entity);
        input.deltaTime = deltaTime;
        renderContext_->processInput(deltaTime, transform.rotation, player, input);

        if (input.jump) {
            actor_simulation::refreshGrounded(voxelWorld, registry, entity);
        }
        actor_simulation::applyControllerInput(registry, entity, deltaTime, false);
        actor_simulation::simulatePhysics(voxelWorld, registry, entity, deltaTime);
        break;
    }
}

RenderSystem::RenderSystem(RenderContext* renderContext, ClientChunkManager* chunkManager, uint32_t localSessionId)
    : renderContext_(renderContext), chunkManager_(chunkManager), localSessionId_(localSessionId) {
}

void RenderSystem::update(VoxelWorld& voxelWorld, ActorWorld& actorWorld, float deltaTime) {
    MW_PROFILE_SCOPE("Client.Render");

    if (!renderContext_ || !chunkManager_) {
        return;
    }

    auto& registry = actorWorld.registry();
    auto view = registry.view<SessionComponent, TransformComponent, PlayerComponent>();
    for (auto entity : view) {
        const auto& session = view.get<SessionComponent>(entity);
        if (session.sessionId != localSessionId_) {
            continue;
        }
        const auto& transform = view.get<TransformComponent>(entity);
        const auto& player = view.get<PlayerComponent>(entity);
        renderContext_->setCamera(transform.position, transform.rotation.y, transform.rotation.x, player.mode, localSessionId_);
        break;
    }

    renderContext_->render(voxelWorld, actorWorld, *chunkManager_);
}
