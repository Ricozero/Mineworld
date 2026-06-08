#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

class ClientWorld;
class RenderContext;
class ServerWorld;
struct PredictedInput;

void applyControllerInput(entt::registry& registry, entt::entity entity, float deltaTime, bool consumeJump);
void simulateServerActor(ServerWorld& world, entt::registry& registry, entt::entity entity, float deltaTime);
void simulateClientActor(ClientWorld& world, entt::registry& registry, entt::entity entity, float deltaTime);
void applyClientPredictedInput(ClientWorld& world, entt::registry& registry, entt::entity entity, const PredictedInput& predictedInput);

class ClientSystem {
public:
    virtual ~ClientSystem() = default;
    virtual void update(ClientWorld& world, float deltaTime) = 0;
};

class ServerSystem {
public:
    virtual ~ServerSystem() = default;
    virtual void update(ServerWorld& world, float deltaTime) = 0;
};

class InputSystem : public ClientSystem {
public:
    InputSystem(RenderContext* renderContext, uint32_t localSessionId);
    void update(ClientWorld& world, float deltaTime) override;

    bool hasInputChanged() const { return inputChanged_; }
    void clearInputChanged() { inputChanged_ = false; }
    bool hasPendingInput() const { return pendingInput_; }
    void clearPendingInput() { pendingInput_ = false; }

private:
    RenderContext* renderContext_ = nullptr;
    uint32_t localSessionId_ = 0;
    bool inputChanged_ = false;
    bool pendingInput_ = false;
};

class PhysicsSystem : public ServerSystem {
public:
    void update(ServerWorld& world, float deltaTime) override;

private:
    void applyGravity(entt::registry& registry, float deltaTime);
    void updateMovement(ServerWorld& world, float deltaTime);
    void moveWithCollision(ServerWorld& world, entt::entity entity, float deltaTime);
};

class RenderSystem : public ClientSystem {
public:
    RenderSystem(RenderContext* renderContext, uint32_t localSessionId);
    void update(ClientWorld& world, float deltaTime) override;

private:
    RenderContext* renderContext_ = nullptr;
    uint32_t localSessionId_ = 0;
};
