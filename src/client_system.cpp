#include "client_system.h"

#include "client_world.h"
#include "entity.h"
#include "common_system.h"
#include "profiler.h"
#include "render_context.h"
#include "server_system.h"

void simulateClientActor(ClientWorld& world, entt::registry& registry, entt::entity entity, float deltaTime) {
    common_system::simulateActorPhysics(world, registry, entity, deltaTime);
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
        common_system::refreshGrounded(world, registry, entity);
    }
    applyControllerInput(registry, entity, predictedInput.deltaTime, false);
    simulateClientActor(world, registry, entity, predictedInput.deltaTime);
}

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
