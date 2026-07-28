#include "client_system.h"

#include "client_world.h"
#include "common_system.h"
#include "entity.h"
#include "profiler.h"
#include "render_context.h"

InputSystem::InputSystem(RenderContext* renderContext, uint32_t localSessionId)
    : renderContext_(renderContext), localSessionId_(localSessionId) {
}

void InputSystem::update(ClientWorld& world, float deltaTime) {
    MW_PROFILE_SCOPE("Client.Input");

    if (!renderContext_) {
        return;
    }

    auto& registry = world.getActorWorld().registry();
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
        const glm::vec3 previousRotation = transform.rotation;
        const PlayerMode previousMode = player.mode;
        const ControllerInputComponent previousInput = input;
        renderContext_->processInput(deltaTime, transform.rotation, player, input);

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

    renderContext_->render(world);
}
