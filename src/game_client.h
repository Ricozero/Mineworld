#pragma once

#include <asio.hpp>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "net_channel.h"
#include "net_protocol.h"
#include "client_world.h"

class ClientSystem;

class GameClient {
public:
    GameClient();
    ~GameClient();

    ClientWorld& world() { return world_; }
    const ClientWorld& world() const { return world_; }

    void registerSystem(std::unique_ptr<ClientSystem> system);
    void update(float deltaTime);

private:
    void pumpNetwork();
    void replaySnapshots();
    void applySnapshot(const NetSnapshot& snapshot);

    static constexpr uint16_t DEFAULT_CLIENT_PORT = 40001;
    static constexpr uint16_t DEFAULT_SERVER_PORT = 40000;
    static constexpr uint32_t DEFAULT_CONV = 114514;

    ClientWorld world_;
    std::vector<std::unique_ptr<ClientSystem>> systems_;

    asio::io_context ioContext_;
    std::unique_ptr<IPacketChannel> channel_;
    std::deque<NetSnapshot> snapshotBuffer_;
    uint32_t lastAppliedSnapshot_ = 0;
};
