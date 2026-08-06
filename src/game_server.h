#pragma once

#include <flatbuffers/flatbuffers.h>

#include <asio.hpp>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "net_interface.h"
#include "net_protocol.h"
#include "server_world.h"

namespace common_system {
template <typename World>
class BaseSystem;
}

class GameServer {
public:
    GameServer();
    ~GameServer();

    ServerWorld& world() { return world_; }
    const ServerWorld& world() const { return world_; }

    void registerSystem(std::unique_ptr<common_system::BaseSystem<ServerWorld>> system);
    void update(float deltaTime);

    entt::entity createLocalPlayer(const std::string& name, uint32_t sessionId, glm::vec3 position, PlayerMode mode);
    entt::entity createRobot(const std::string& name, glm::vec3 position);
    bool loadChunk(glm::ivec3 chunkPos);
    bool unloadChunk(glm::ivec3 chunkPos);
    void setBlock(glm::ivec3 worldPos, BlockData blockData);

private:
    struct Session {
        uint32_t sessionId = 0;
        uint32_t snapshotSequence = 0;
        float snapshotTimer = 0.0f;
        bool initialSnapshotSent = false;
        bool helloReceived = false;
        bool ready = false;
        uint32_t lastProcessedInputSequence = 0;
        std::string actorName;

        glm::ivec3 lastChunkPos{INT_MAX, INT_MAX, INT_MAX};
        std::unordered_set<glm::ivec3> cachedVisibleChunks;

        std::unordered_map<glm::ivec3, NetChunkState> pendingChunkUpdates;

        flatbuffers::FlatBufferBuilder snapshotBuilder{8192};
    };

    Session& getOrCreateSession(uint32_t sessionId);
    NetSnapshot buildSnapshot(Session& session, bool forceFullChunkState);
    void updateVisibleChunks(float deltaTime);
    void updateSessionVisibleChunks(Session& session);
    void queueChunkUpdate(Session& session, NetChunkState chunkState);

    NetChunkState buildLoadedChunkState(glm::ivec3 chunkPos);
    void pumpNetwork();

    void onSessionConnect(uint32_t sessionId);
    void onSessionDisconnect(uint32_t sessionId);
    bool onSessionPacket(uint32_t sessionId, const std::vector<uint8_t>& packet);
    bool onClientHello(uint32_t sessionId);
    void onClientReady(uint32_t sessionId);
    void onClientInput(uint32_t sessionId, const NetClientInput& input);

    ServerWorld world_;
    std::vector<std::unique_ptr<common_system::BaseSystem<ServerWorld>>> systems_;

    asio::io_context ioContext_;

    std::unique_ptr<INetServer> netServer_;
    std::unordered_map<uint32_t, Session> sessions_;
    std::unordered_map<glm::ivec3, float> chunkUnloadTimers_;
    uint32_t nextPlayerIndex_ = 1;
};
