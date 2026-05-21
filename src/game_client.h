#pragma once

#include <asio.hpp>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "client_world.h"
#include "net_channel.h"
#include "net_protocol.h"

class ClientSystem;
class RenderContext;

class GameClient {
public:
    explicit GameClient(RenderContext* renderContext = nullptr, const std::string& spectatorName = "");
    ~GameClient();

    ClientWorld& world() { return world_; }
    const ClientWorld& world() const { return world_; }

    const std::string& spectatorName() const { return spectatorName_; }

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
    std::string spectatorName_;

    asio::io_context ioContext_;
    std::unique_ptr<IPacketChannel> channel_;
    std::deque<NetSnapshot> snapshotBuffer_;
    uint32_t lastAppliedSnapshot_ = 0;
    RenderContext* renderContext_ = nullptr;
};