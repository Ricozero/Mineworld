#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

class ClientWorld;
class RenderContext;
class ServerWorld;

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
    explicit InputSystem(RenderContext* renderContext = nullptr, uint32_t localSessionId = 0);
    void update(ClientWorld& world, float deltaTime) override;

    bool hasInputChanged() const { return inputChanged_; }
    void clearInputChanged() { inputChanged_ = false; }

private:
    RenderContext* renderContext_ = nullptr;
    uint32_t localSessionId_ = 0;
    bool inputChanged_ = false;
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
    explicit RenderSystem(RenderContext* renderContext = nullptr, uint32_t localSessionId = 0);
    void update(ClientWorld& world, float deltaTime) override;

private:
    RenderContext* renderContext_ = nullptr;
    uint32_t localSessionId_ = 0;
};
