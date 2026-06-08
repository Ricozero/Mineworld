#pragma once

#include <asio.hpp>
#include <cstdint>
#include <deque>
#include <entt/entt.hpp>
#include <memory>
#include <string>
#include <vector>

#include "client_world.h"
#include "net_channel.h"
#include "net_protocol.h"

class ClientSystem;
class RenderContext;
class InputSystem;

class GameClient {
public:
    GameClient(RenderContext* renderContext, std::string serverAddress, uint16_t serverPort);
    ~GameClient();

    ClientWorld& world() { return world_; }
    const ClientWorld& world() const { return world_; }

    uint32_t localSessionId() const { return localSessionId_; }
    bool isSessionReady() const { return sessionReady_; }

    void registerSystem(std::unique_ptr<ClientSystem> system);
    void update(float deltaTime);
    void disconnect();

private:
    void pumpNetwork();
    void handleServerHello(const NetServerHello& hello);
    void sendInputToServer();
    void replaySnapshots();
    void applySnapshot(const NetSnapshot& snapshot);
    void reconcileLocalActor(entt::registry& registry, entt::entity entity, const NetActorState& actor);
    void queueRemoteActorSample(entt::registry& registry, entt::entity entity, const NetActorState& actor);
    void updateRemoteInterpolation(float deltaTime);

    ClientWorld world_;
    std::vector<std::unique_ptr<ClientSystem>> systems_;
    InputSystem* inputSystem_ = nullptr;
    uint32_t localSessionId_ = 0;
    bool sessionReady_ = false;

    asio::io_context ioContext_;
    std::unique_ptr<IPacketChannel> channel_;
    std::deque<NetSnapshot> snapshotBuffer_;
    uint32_t lastAppliedSnapshot_ = 0;
    RenderContext* renderContext_ = nullptr;
    bool helloPending_ = true;
    bool disconnectSent_ = false;
    double snapshotClock_ = 0.0;
};
