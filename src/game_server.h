#pragma once

#include <flatbuffers/flatbuffers.h>

#include <asio.hpp>
#include <cstdint>
#include <deque>
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "net_channel.h"
#include "net_protocol.h"
#include "server_world.h"

class ServerSystem;

class GameServer {
public:
    GameServer();
    ~GameServer();

    ServerWorld& world() { return world_; }
    const ServerWorld& world() const { return world_; }

    void registerSystem(std::unique_ptr<ServerSystem> system);
    void update(float deltaTime);

    entt::entity createPlayer(
        const std::string& name,
        uint32_t sessionId,
        glm::vec3 position = glm::vec3(0.0f),
        PlayerMode mode = PlayerMode::Survival);
    entt::entity createRobot(const std::string& name, glm::vec3 position = glm::vec3(0.0f));
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
        std::string actorName;

        glm::ivec3 lastChunkPos{INT_MAX, INT_MAX, INT_MAX};
        std::unordered_set<glm::ivec3> cachedVisibleChunks;

        std::vector<glm::ivec3> pendingDirtyChunks;
        std::vector<NetChunkState> pendingChunkUpdates;
        std::deque<NetBlockState> pendingBlockUpdates;

        flatbuffers::FlatBufferBuilder snapshotBuilder{8192};
    };

    Session& getOrCreateSession(uint32_t sessionId);
    NetSnapshot buildSnapshot(Session& session, bool forceFullChunkState);
    void updateVisibleChunks();
    void updateSessionVisibleChunks(Session& session);

    void queueChunkBlockSnapshot(glm::ivec3 chunkPos, Session& session);
    void pumpNetwork();

    void onSessionConnect(uint32_t sessionId);
    void onSessionPacket(uint32_t sessionId, const std::vector<uint8_t>& packet);
    void onClientHello(uint32_t sessionId);
    void onClientInput(uint32_t sessionId, const NetClientInput& input);

    ServerWorld world_;
    std::vector<std::unique_ptr<ServerSystem>> systems_;

    asio::io_context ioContext_;

    std::unique_ptr<IPacketServer> server_;
    std::unordered_map<uint32_t, Session> sessions_;
    uint32_t nextPlayerIndex_ = 0;
};
