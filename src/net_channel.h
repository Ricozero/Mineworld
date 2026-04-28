#pragma once

#include <vector>

#include <asio.hpp>

class IPacketChannel {
public:
    using Endpoint = asio::ip::udp::endpoint;

    virtual ~IPacketChannel() = default;

    virtual void setRemote(const Endpoint& endpoint) = 0;
    virtual bool hasRemote() const = 0;
    virtual void sendReliable(const std::vector<uint8_t>& payload) = 0;
    virtual void pump() = 0;
    virtual bool popPacket(std::vector<uint8_t>& outPacket) = 0;
};
