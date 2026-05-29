#pragma once

#include <asio.hpp>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "net_channel.h"
#include "net_protocol.h"
#include "server_world.h"

class ServerSystem;

enum class EntryMode {
    Spectator,
    Player,
};

class GameServer {
public:
    explicit GameServer(EntryMode entryMode = EntryMode::Spectator);
    ~GameServer();

    ServerWorld& world() { return world_; }
    const ServerWorld& world() const { return world_; }

    void registerSystem(std::unique_ptr<ServerSystem> system);
    void update(float deltaTime);

    entt::entity createPlayer(const std::string& name, uint32_t sessionId, glm::vec3 position = glm::vec3(0.0f));
    entt::entity createRobot(const std::string& name, glm::vec3 position = glm::vec3(0.0f));
    entt::entity createSpectator(const std::string& name, uint32_t sessionId, glm::vec3 position = glm::vec3(0.0f));
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
        std::vector<NetChunkState> pendingChunkUpdates;
        std::deque<NetBlockState> pendingBlockUpdates;
    };

    Session& getOrCreateSession(uint32_t sessionId);
    NetSnapshot buildSnapshot(Session& session, bool forceFullChunkState);
    void updateVisibleChunks();

    void queueChunkBlockSnapshot(glm::ivec3 chunkPos, Session& session);
    void queueLoadedBlockSnapshots(Session& session, const std::unordered_set<glm::ivec3>& visibleChunks = {});
    void pumpNetwork();

    void onSessionConnect(uint32_t sessionId);
    void onSessionPacket(uint32_t sessionId, const std::vector<uint8_t>& packet);
    void onClientHello(uint32_t sessionId);
    void onClientInput(uint32_t sessionId, const NetClientInput& input);

    ServerWorld world_;
    std::vector<std::unique_ptr<ServerSystem>> systems_;
    EntryMode entryMode_ = EntryMode::Spectator;

    asio::io_context ioContext_;

    std::unique_ptr<IPacketServer> server_;
    std::unordered_map<uint32_t, Session> sessions_;
    uint32_t nextPlayerIndex_ = 0;
};