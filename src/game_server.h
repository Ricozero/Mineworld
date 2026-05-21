#pragma once

#include <asio.hpp>
#include <cstdint>
#include <deque>
#include <memory>
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

    entt::entity createPlayer(const std::string& name, glm::vec3 position = glm::vec3(0.0f));
    entt::entity createSpectator(const std::string& name, glm::vec3 position = glm::vec3(0.0f));
    bool loadChunk(glm::ivec3 chunkPos);
    bool unloadChunk(glm::ivec3 chunkPos);
    void setBlock(glm::ivec3 worldPos, BlockData blockData);

private:
    NetSnapshot buildSnapshot(bool forceFullChunkState);
    void updateVisibleChunks();
    void queueChunkBlockSnapshot(glm::ivec3 chunkPos);
    void queueLoadedBlockSnapshots();
    void pumpNetwork();
    void onClientPacket(const std::vector<uint8_t>& packet);

    static constexpr uint16_t DEFAULT_SERVER_PORT = 40000;
    static constexpr uint32_t DEFAULT_CONV = 114514;

    ServerWorld world_;
    std::vector<std::unique_ptr<ServerSystem>> systems_;

    asio::io_context ioContext_;
    std::unique_ptr<IPacketChannel> channel_;
    uint32_t snapshotSequence_ = 0;
    float snapshotTimer_ = 0.0f;
    bool initialSnapshotSent_ = false;
    std::vector<NetChunkState> pendingChunkUpdates_;
    std::deque<NetBlockState> pendingBlockUpdates_;
};