#pragma once

#include <asio.hpp>
#include <cstdint>
#include <deque>
#include <entt/entt.hpp>
#include <memory>
#include <string>
#include <vector>

#include "actor_world.h"
#include "chunk_mesh.h"
#include "client_chunk_manager.h"
#include "system.h"
#include "voxel_world.h"
#include "net_interface.h"
#include "net_protocol.h"

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

    uint32_t localSessionId() const { return localSessionId_; }
    bool isSessionReady() const { return state_ == State::Running; }
    bool hasFailed() const { return state_ == State::Failed; }
    State state() const { return state_; }
    std::string statusText() const;

    void registerSystem(std::unique_ptr<System> system);
    void update(float deltaTime);
    void disconnect();

private:
    void pumpNetwork();
    void onServerPacket(const std::vector<uint8_t>& packet);
    void handleServerHello(const NetServerHello& hello);
    void tryEnterRunning();
    void fail(std::string reason);
    void sendInputToServer();
    void replayEntitySnapshots();
    void applyEntitySnapshot(const NetEntitySnapshot& snapshot);
    void applyChunkUpdate(NetChunkUpdate&& update);
    void rebuildChunkMeshes();
    ClientChunkManager::MeshFocus localPlayerMeshFocus() const;
    void queueRemoteActorSample(entt::registry& registry, entt::entity entity, const NetActorState& actor);
    void updateRemoteInterpolation(float deltaTime);

    VoxelWorld voxelWorld_;
    ActorWorld actorWorld_{false};
    ClientChunkManager chunkManager_{voxelWorld_};
    ChunkMesh meshScratch_;
    std::vector<std::unique_ptr<System>> systems_;
    uint32_t localSessionId_ = 0;
    bool meshPoolExhausted_ = false;
    State state_ = State::Connecting;
    std::string failureReason_;
    float secondsSincePacket_ = 0.0f;

    asio::io_context ioContext_;
    std::unique_ptr<INetClient> netClient_;
    std::deque<NetEntitySnapshot> entitySnapshotBuffer_;
    uint32_t lastAppliedEntitySnapshot_ = 0;
    RenderContext* renderContext_ = nullptr;
    bool helloPending_ = true;
    bool disconnectSent_ = false;
    double snapshotClock_ = 0.0;
    uint32_t nextInputSequence_ = 1;
};
