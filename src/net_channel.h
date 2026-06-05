#pragma once

#include <asio.hpp>
#include <cstdint>
#include <functional>
#include <vector>

class IPacketChannel {
public:
    using Endpoint = asio::ip::udp::endpoint;

    virtual ~IPacketChannel() = default;

    virtual void setRemote(const Endpoint& endpoint) = 0;
    virtual bool hasRemote() const = 0;
    virtual void sendReliable(const std::vector<uint8_t>& payload) = 0;
    virtual void flush() = 0;
    virtual void pump() = 0;
    virtual bool popPacket(std::vector<uint8_t>& outPacket) = 0;
};

// Callback when a new session is established. sessionId is assigned by the server.
using SessionConnectCallback = std::function<void(uint32_t sessionId)>;
// Callback when a packet is received from a specific session. Return false to close the session.
using SessionPacketCallback = std::function<bool(uint32_t sessionId, const std::vector<uint8_t>& packet)>;
// Callback when a session is considered disconnected.
using SessionDisconnectCallback = std::function<void(uint32_t sessionId)>;

class IPacketServer {
public:
    virtual ~IPacketServer() = default;

    virtual void setOnConnect(SessionConnectCallback callback) = 0;
    virtual void setOnPacket(SessionPacketCallback callback) = 0;
    virtual void setOnDisconnect(SessionDisconnectCallback callback) = 0;
    virtual void sendTo(uint32_t sessionId, const std::vector<uint8_t>& payload) = 0;
    virtual void pump() = 0;
    virtual bool hasSession(uint32_t sessionId) const = 0;
    virtual std::vector<uint32_t> getSessionIds() const = 0;
};
