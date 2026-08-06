#pragma once

#include <asio.hpp>
#include <cstdint>
#include <functional>
#include <vector>

class INetClient {
public:
    using Endpoint = asio::ip::udp::endpoint;

    virtual ~INetClient() = default;

    virtual void connect(const Endpoint& endpoint) = 0;
    virtual bool isReady() const = 0;
    virtual void sendReliable(const std::vector<uint8_t>& payload) = 0;
    virtual void flush() = 0;
    virtual void pump() = 0;
    virtual bool popPacket(std::vector<uint8_t>& outPacket) = 0;
};

using SessionConnectCallback = std::function<void(uint32_t sessionId)>;
using SessionPacketCallback = std::function<bool(uint32_t sessionId, const std::vector<uint8_t>& packet)>;
using SessionDisconnectCallback = std::function<void(uint32_t sessionId)>;

class INetServer {
public:
    virtual ~INetServer() = default;

    virtual void setOnConnect(SessionConnectCallback callback) = 0;
    virtual void setOnPacket(SessionPacketCallback callback) = 0;
    virtual void setOnDisconnect(SessionDisconnectCallback callback) = 0;
    virtual void sendTo(uint32_t sessionId, const std::vector<uint8_t>& payload) = 0;
    virtual void pump() = 0;
    virtual bool hasSession(uint32_t sessionId) const = 0;
    virtual std::vector<uint32_t> getSessionIds() const = 0;
};
