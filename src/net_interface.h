#pragma once

#include <asio.hpp>
#include <cstdint>
#include <span>
#include <vector>

enum class NetEventType {
    Connected,
    Packet,
    Disconnected,
};

struct NetEvent {
    NetEventType type = NetEventType::Packet;
    uint32_t sessionId = 0;
    std::vector<uint8_t> payload;
};

class INetClient {
public:
    using Endpoint = asio::ip::udp::endpoint;

    virtual ~INetClient() = default;

    virtual void connect(const Endpoint& endpoint) = 0;
    virtual bool isConnected() const = 0;
    virtual bool send(std::span<const uint8_t> payload) = 0;
    virtual void flush() = 0;
    virtual void pump() = 0;
    virtual bool popEvent(NetEvent& outEvent) = 0;
    virtual void close() = 0;
};

class INetServer {
public:
    virtual ~INetServer() = default;

    virtual bool send(uint32_t sessionId, std::span<const uint8_t> payload) = 0;
    virtual void flush() = 0;
    virtual void pump() = 0;
    virtual bool popEvent(NetEvent& outEvent) = 0;
    virtual void close(uint32_t sessionId) = 0;
    virtual bool hasSession(uint32_t sessionId) const = 0;
};
