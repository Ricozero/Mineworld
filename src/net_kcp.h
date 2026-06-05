#pragma once

#include <ikcp.h>

#include <asio.hpp>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "net_channel.h"

class KcpChannel : public IPacketChannel {
public:
    using Udp = asio::ip::udp;

    KcpChannel(asio::io_context& ioContext, uint16_t localPort);
    ~KcpChannel() override;

    void setRemote(const Endpoint& endpoint) override;
    bool hasRemote() const override { return remote_.has_value(); }

    void sendReliable(const std::vector<uint8_t>& payload) override;
    void flush() override;
    void pump() override;
    bool popPacket(std::vector<uint8_t>& outPacket) override;

    /// Initiate handshake with remote server
    void startHandshake();
    /// Returns true once the handshake is complete and KCP is ready
    bool isReady() const { return kcp_ != nullptr; }

private:
    void initKcp(uint32_t conv);
    void sendHandshakeRequest();

    static int kcpOutput(const char* buf, int len, ikcpcb* kcp, void* user);
    int sendRaw(const char* data, size_t size);
    uint32_t nowMs() const;

    asio::io_context& ioContext_;
    Udp::socket socket_;
    ikcpcb* kcp_ = nullptr;

    std::optional<Endpoint> remote_;
    std::vector<uint8_t> recvBuffer_;
    std::vector<std::vector<uint8_t>> recvPackets_;
    bool handshakeComplete_ = false;
};

// A KCP-based server that manages multiple sessions over a single UDP port.
// Each session gets a unique conv (= sessionId) for identification.
class KcpServer : public IPacketServer {
public:
    using Udp = asio::ip::udp;
    using Endpoint = asio::ip::udp::endpoint;

    KcpServer(asio::io_context& ioContext, uint16_t localPort);
    ~KcpServer() override;

    void setOnConnect(SessionConnectCallback callback) override;
    void setOnPacket(SessionPacketCallback callback) override;
    void setOnDisconnect(SessionDisconnectCallback callback) override;
    void sendTo(uint32_t sessionId, const std::vector<uint8_t>& payload) override;
    void pump() override;
    bool hasSession(uint32_t sessionId) const override;
    std::vector<uint32_t> getSessionIds() const override;

private:
    struct SessionState {
        uint32_t sessionId = 0;
        Endpoint remote;
        ikcpcb* kcp = nullptr;
        std::vector<std::vector<uint8_t>> recvPackets;
        uint32_t lastReceiveMs = 0;
        bool pendingDisconnect = false;
    };

    SessionState* findSessionByConv(uint32_t conv);
    SessionState* findSessionByEndpoint(const Endpoint& endpoint);
    SessionState& createSession(const Endpoint& endpoint);
    void processReceivedPackets(SessionState& session);
    void handleHandshake(const Endpoint& sender);
    void destroySession(uint32_t sessionId, bool notify);
    void removeTimedOutSessions(uint32_t now);

    static int kcpOutput(const char* buf, int len, ikcpcb* kcp, void* user);
    int sendRawTo(const char* data, size_t size, const Endpoint& endpoint);
    uint32_t nowMs() const;

    struct KcpOutputContext {
        KcpServer* server = nullptr;
        uint32_t sessionId = 0;
    };

    asio::io_context& ioContext_;
    Udp::socket socket_;
    std::vector<uint8_t> recvBuffer_;

    uint32_t nextSessionId_ = 1;
    std::unordered_map<uint32_t, SessionState> sessions_;
    std::unordered_map<uint32_t, KcpOutputContext> outputContexts_;

    SessionConnectCallback onConnect_;
    SessionPacketCallback onPacket_;
    SessionDisconnectCallback onDisconnect_;
};
