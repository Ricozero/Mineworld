#pragma once

#include <asio.hpp>
#include <cstdint>
#include <memory>
#include <vector>

#include "net_kcp.h"
#include "net_protocol.h"
#include "world.h"

class BaseSystem;

class GameServer {
public:
    GameServer();
    ~GameServer();

    World& world() { return world_; }
    const World& world() const { return world_; }

    void registerSystem(std::unique_ptr<BaseSystem> system);
    void update(float deltaTime);

    bool loadChunk(glm::ivec3 chunkPos);
    bool unloadChunk(glm::ivec3 chunkPos);
    void setBlock(glm::ivec3 worldPos, BlockData blockData);

private:
    NetSnapshot buildSnapshot(bool forceFullChunkState);
    void pumpNetwork();
    void onClientPacket(const std::vector<uint8_t>& packet);

    static constexpr uint16_t DEFAULT_SERVER_PORT = 40000;
    static constexpr uint32_t DEFAULT_CONV = 114514;

    World world_;
    std::vector<std::unique_ptr<BaseSystem>> systems_;

    asio::io_context ioContext_;
    std::unique_ptr<KcpChannel> channel_;
    uint32_t snapshotSequence_ = 0;
    float snapshotTimer_ = 0.0f;
    bool initialSnapshotSent_ = false;
    std::vector<NetChunkState> pendingChunkUpdates_;
    std::vector<NetBlockState> pendingBlockUpdates_;
};
