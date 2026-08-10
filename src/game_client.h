#pragma once

#include <asio.hpp>
#include <cstdint>
#include <deque>
#include <entt/entt.hpp>
#include <glm/gtx/hash.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "client_world.h"
#include "net_interface.h"
#include "net_protocol.h"

namespace common_system {
template <typename World>
class BaseSystem;
}
class RenderContext;

class GameClient {
public:
    enum class State {
        Connecting,
        Awaiting,
        Loading,
        Running,
        Disconnecting,
        Failed,
    };

    GameClient(RenderContext* renderContext, std::string address, uint16_t port);
    ~GameClient();

    ClientWorld& world() { return world_; }
    const ClientWorld& world() const { return world_; }

    uint32_t localSessionId() const { return localSessionId_; }
    bool isSessionReady() const { return state_ == State::Running; }
    bool hasFailed() const { return state_ == State::Failed; }
    State state() const { return state_; }
    std::string statusText() const;

    void registerSystem(std::unique_ptr<common_system::BaseSystem<ClientWorld>> system);
    void update(float deltaTime);
    void disconnect();

private:
    void pumpNetwork();
    void onServerPacket(const std::vector<uint8_t>& packet);
    void handleServerHello(const NetServerHello& hello);
    bool areCoreChunksLoaded() const;
    void tryEnterRunning();
    void fail(std::string reason);
    void sendInputToServer();
    void replayEntitySnapshots();
    void applyEntitySnapshot(const NetEntitySnapshot& snapshot);
    void queueChunkUpdate(NetChunkUpdate update);
    void applyPendingChunkUpdates();
    bool isCoreChunk(glm::ivec3 chunkPos) const;
    void queueRemoteActorSample(entt::registry& registry, entt::entity entity, const NetActorState& actor);
    void updateRemoteInterpolation(float deltaTime);

    ClientWorld world_;
    std::vector<std::unique_ptr<common_system::BaseSystem<ClientWorld>>> systems_;
    uint32_t localSessionId_ = 0;
    State state_ = State::Connecting;
    std::string failureReason_;
    std::vector<glm::ivec3> coreChunks_;
    float secondsSincePacket_ = 0.0f;

    asio::io_context ioContext_;
    std::unique_ptr<INetClient> netClient_;
    std::deque<NetEntitySnapshot> entitySnapshotBuffer_;
    std::unordered_map<glm::ivec3, NetChunkUpdate> pendingChunkUpdates_;
    uint32_t lastAppliedEntitySnapshot_ = 0;
    RenderContext* renderContext_ = nullptr;
    bool helloPending_ = true;
    bool disconnectSent_ = false;
    double snapshotClock_ = 0.0;
    uint32_t nextInputSequence_ = 1;
};
