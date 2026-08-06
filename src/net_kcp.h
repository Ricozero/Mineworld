#pragma once

#include <ikcp.h>

#include <asio.hpp>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

#include "net_interface.h"

class KcpClient final : public INetClient {
public:
    using Udp = asio::ip::udp;

    KcpClient(asio::io_context& ioContext, uint16_t localPort);
    ~KcpClient() override;

    void connect(const Endpoint& endpoint) override;
    bool isReady() const override { return kcp_ != nullptr; }
    void sendReliable(const std::vector<uint8_t>& payload) override;
    void flush() override;
    void pump() override;
    bool popPacket(std::vector<uint8_t>& outPacket) override;

private:
    void reset();
    void initKcp(uint32_t conv);
    void sendHandshakeRequest(uint32_t now);

    static int kcpOutput(const char* buf, int len, ikcpcb* kcp, void* user);
    int sendRaw(const char* data, size_t size);
    uint32_t nowMs() const;

    Udp::socket socket_;
    ikcpcb* kcp_ = nullptr;

    std::optional<Endpoint> remote_;
    std::deque<std::vector<uint8_t>> recvPackets_;
    std::vector<uint8_t> recvBuffer_;
    uint64_t handshakeNonce_ = 0;
    uint32_t lastHandshakeSendMs_ = 0;
    bool handshakeStarted_ = false;
    bool versionMismatchLogged_ = false;
};

class KcpServer final : public INetServer {
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
    struct KcpOutputContext {
        KcpServer* server = nullptr;
        uint32_t sessionId = 0;
    };

    struct SessionState {
        uint32_t sessionId = 0;
        Endpoint remote;
        uint64_t handshakeNonce = 0;
        ikcpcb* kcp = nullptr;
        uint32_t lastReceiveMs = 0;
        bool pendingDisconnect = false;
        KcpOutputContext outputContext;
    };

    struct HandshakeRateState {
        uint32_t windowStartMs = 0;
        uint32_t attempts = 0;
        uint32_t lastSeenMs = 0;
    };

    std::optional<uint32_t> allocateSessionId();
    SessionState* findSessionByConv(uint32_t conv);
    SessionState* findSessionByEndpoint(const Endpoint& endpoint);
    SessionState* createSession(const Endpoint& endpoint, uint64_t handshakeNonce);
    void processReceivedPackets(SessionState& session);
    void handleHandshake(const Endpoint& sender, uint64_t handshakeNonce, uint32_t now);
    void sendHandshakeResponse(const SessionState& session);
    void destroySession(uint32_t sessionId, bool notify);
    void removeTimedOutSessions(uint32_t now);
    bool allowHandshake(const Endpoint& sender, uint32_t now);
    void cleanupHandshakeRateLimits(uint32_t now);
    void evictStalestHandshakeRateState();

    static int kcpOutput(const char* buf, int len, ikcpcb* kcp, void* user);
    int sendRawTo(const char* data, size_t size, const Endpoint& endpoint);
    uint32_t nowMs() const;

    Udp::socket socket_;
    std::vector<uint8_t> recvBuffer_;

    uint32_t nextSessionId_ = 1;
    std::unordered_map<uint32_t, SessionState> sessions_;
    std::unordered_map<Endpoint, uint32_t> endpointSessions_;
    std::unordered_map<asio::ip::address, HandshakeRateState> handshakeRateStates_;
    uint32_t lastRateLimitCleanupMs_ = 0;

    SessionConnectCallback onConnect_;
    SessionPacketCallback onPacket_;
    SessionDisconnectCallback onDisconnect_;
};
