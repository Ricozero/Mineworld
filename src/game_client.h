#pragma once

#include <asio.hpp>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "net_kcp.h"
#include "net_protocol.h"
#include "world.h"

class BaseSystem;

class GameClient {
public:
    GameClient();
    ~GameClient();

    World& world() { return world_; }
    const World& world() const { return world_; }

    void registerSystem(std::unique_ptr<BaseSystem> system);
    void update(float deltaTime);

private:
    void pumpNetwork();
    void replaySnapshots();
    void applySnapshot(const NetSnapshot& snapshot);

    static constexpr uint16_t DEFAULT_CLIENT_PORT = 40001;
    static constexpr uint16_t DEFAULT_SERVER_PORT = 40000;
    static constexpr uint32_t DEFAULT_CONV = 114514;

    World world_;
    std::vector<std::unique_ptr<BaseSystem>> systems_;

    asio::io_context ioContext_;
    std::unique_ptr<KcpChannel> channel_;
    std::deque<NetSnapshot> snapshotBuffer_;
    uint32_t lastAppliedSnapshot_ = 0;
};
