#include "net_kcp.h"

#include <chrono>
#include <cstring>

#include "helper.h"
#include "log.h"

namespace {

constexpr int kRecvMtu = 1400;
constexpr uint32_t kMagic = 0x4B435048;
constexpr size_t kRequestSize = 4;
constexpr size_t kResponseSize = 8;
constexpr uint32_t kSessionTimeoutMs = 10'000;

bool isUdpPeerReset(asio::error_code ec) {
    return ec == asio::error::connection_reset;
}

}  // namespace

// Handshake protocol constants
namespace kcp_handshake {
}  // namespace kcp_handshake

// ============================================================
// KcpChannel (client-side, single connection)
// ============================================================

KcpChannel::KcpChannel(asio::io_context& ioContext, uint16_t localPort)
    : ioContext_(ioContext), socket_(ioContext, Udp::endpoint(Udp::v4(), localPort)), recvBuffer_(kRecvMtu) {
    socket_.non_blocking(true);
}

KcpChannel::~KcpChannel() {
    if (kcp_) {
        ikcp_release(kcp_);
        kcp_ = nullptr;
    }
}

void KcpChannel::setRemote(const Endpoint& endpoint) {
    remote_ = endpoint;
}

void KcpChannel::startHandshake() {
    if (!remote_) {
        logging::warn("Cannot start handshake: no remote set");
        return;
    }
    sendHandshakeRequest();
}

void KcpChannel::sendHandshakeRequest() {
    uint8_t buf[kRequestSize];
    uint32_t magic = kMagic;
    std::memcpy(buf, &magic, 4);

    asio::error_code ec;
    socket_.send_to(asio::buffer(buf, sizeof(buf)), *remote_, 0, ec);
    if (ec) {
        logging::warn("Failed to send handshake request: {}", ec.message());
    }
}

void KcpChannel::initKcp(uint32_t conv) {
    kcp_ = ikcp_create(conv, this);
    if (!kcp_) {
        crash("ikcp_create failed");
    }

    ikcp_setoutput(kcp_, &KcpChannel::kcpOutput);
    ikcp_nodelay(kcp_, 1, 10, 2, 1);
    ikcp_wndsize(kcp_, 256, 256);
    ikcp_setmtu(kcp_, 1200);
    kcp_->rx_minrto = 10;

    handshakeComplete_ = true;
    logging::info("Handshake complete, conv = {}", conv);
}

void KcpChannel::sendReliable(const std::vector<uint8_t>& payload) {
    if (!kcp_ || !remote_) {
        return;
    }
    const int result = ikcp_send(kcp_, reinterpret_cast<const char*>(payload.data()), static_cast<int>(payload.size()));
    if (result < 0) {
        logging::warn("ikcp_send failed: {}", result);
    }
}

void KcpChannel::flush() {
    if (!kcp_) {
        return;
    }
    ikcp_update(kcp_, nowMs());
    ikcp_flush(kcp_);
}

void KcpChannel::pump() {
    for (;;) {
        Udp::endpoint sender;
        asio::error_code ec;
        const auto received = socket_.receive_from(asio::buffer(recvBuffer_), sender, 0, ec);
        if (ec == asio::error::would_block || ec == asio::error::try_again) {
            break;
        }
        if (isUdpPeerReset(ec)) {
            break;
        }
        if (ec) {
            logging::warn("UDP receive error: {}", ec.message());
            break;
        }
        if (!remote_) {
            remote_ = sender;
        }
        if (*remote_ != sender) {
            continue;
        }

        // Check if this is a handshake response
        if (!handshakeComplete_ && received == kResponseSize) {
            uint32_t magic = 0;
            uint32_t conv = 0;
            std::memcpy(&magic, recvBuffer_.data(), 4);
            std::memcpy(&conv, recvBuffer_.data() + 4, 4);
            if (magic == kMagic) {
                initKcp(conv);
                continue;
            }
        }

        if (!kcp_) {
            // Not yet initialized, resend handshake
            sendHandshakeRequest();
            continue;
        }

        const int inputResult = ikcp_input(kcp_, reinterpret_cast<const char*>(recvBuffer_.data()), static_cast<long>(received));
        if (inputResult < 0) {
            logging::warn("ikcp_input failed: {}", inputResult);
        }
    }

    if (!kcp_) {
        return;
    }

    ikcp_update(kcp_, nowMs());

    for (;;) {
        const int packetSize = ikcp_peeksize(kcp_);
        if (packetSize < 0) {
            break;
        }
        std::vector<uint8_t> packet(static_cast<size_t>(packetSize));
        const int received = ikcp_recv(kcp_, reinterpret_cast<char*>(packet.data()), static_cast<int>(packet.size()));
        if (received < 0) {
            break;
        }
        packet.resize(static_cast<size_t>(received));
        recvPackets_.push_back(std::move(packet));
    }
}

bool KcpChannel::popPacket(std::vector<uint8_t>& outPacket) {
    if (recvPackets_.empty()) {
        return false;
    }
    outPacket = std::move(recvPackets_.front());
    recvPackets_.erase(recvPackets_.begin());
    return true;
}

int KcpChannel::kcpOutput(const char* buf, int len, ikcpcb* kcp, void* user) {
    auto* self = static_cast<KcpChannel*>(user);
    return self->sendRaw(buf, static_cast<size_t>(len));
}

int KcpChannel::sendRaw(const char* data, size_t size) {
    if (!remote_) {
        return -1;
    }
    asio::error_code ec;
    socket_.send_to(asio::buffer(data, size), *remote_, 0, ec);
    if (ec) {
        logging::warn("UDP send error: {}", ec.message());
        return -1;
    }
    return 0;
}

uint32_t KcpChannel::nowMs() const {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// ============================================================
// KcpServer (server-side, multiple sessions over single socket)
// ============================================================

KcpServer::KcpServer(asio::io_context& ioContext, uint16_t localPort)
    : ioContext_(ioContext),
      socket_(ioContext, Udp::endpoint(Udp::v4(), localPort)),
      recvBuffer_(kRecvMtu) {
    socket_.non_blocking(true);
}

KcpServer::~KcpServer() {
    for (auto& [id, session] : sessions_) {
        if (session.kcp) {
            ikcp_release(session.kcp);
            session.kcp = nullptr;
        }
    }
}

void KcpServer::setOnConnect(SessionConnectCallback callback) {
    onConnect_ = std::move(callback);
}

void KcpServer::setOnPacket(SessionPacketCallback callback) {
    onPacket_ = std::move(callback);
}

void KcpServer::setOnDisconnect(SessionDisconnectCallback callback) {
    onDisconnect_ = std::move(callback);
}

void KcpServer::sendTo(uint32_t sessionId, const std::vector<uint8_t>& payload) {
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return;
    }
    auto& session = it->second;
    if (!session.kcp) {
        return;
    }
    const int result = ikcp_send(session.kcp, reinterpret_cast<const char*>(payload.data()), static_cast<int>(payload.size()));
    if (result < 0) {
        logging::warn("ikcp_send to session {} failed: {}", sessionId, result);
    }
}

void KcpServer::pump() {
    // Receive all UDP packets and dispatch to sessions
    for (;;) {
        Udp::endpoint sender;
        asio::error_code ec;
        const auto received = socket_.receive_from(asio::buffer(recvBuffer_), sender, 0, ec);
        if (ec == asio::error::would_block || ec == asio::error::try_again) {
            break;
        }
        if (isUdpPeerReset(ec)) {
            break;
        }
        if (ec) {
            logging::warn("UDP receive error: {}", ec.message());
            break;
        }

        // Check if this is a handshake request
        if (received == kRequestSize) {
            uint32_t magic = 0;
            std::memcpy(&magic, recvBuffer_.data(), 4);
            if (magic == kMagic) {
                handleHandshake(sender);
                continue;
            }
        }

        // Extract conv from KCP packet header (first 4 bytes)
        if (received < 4) {
            continue;
        }
        uint32_t conv = 0;
        std::memcpy(&conv, recvBuffer_.data(), 4);

        SessionState* session = findSessionByConv(conv);
        if (!session) {
            // Unknown conv, ignore
            logging::warn("Received packet with unknown conv {}", conv);
            continue;
        }

        // Update remote endpoint if it changed (connection migration)
        if (session->remote != sender) {
            logging::info("Session {} migrated from {}:{} to {}:{}",
                          session->sessionId,
                          session->remote.address().to_string(), session->remote.port(),
                          sender.address().to_string(), sender.port());
            session->remote = sender;
        }
        session->lastReceiveMs = nowMs();

        const int inputResult = ikcp_input(
            session->kcp,
            reinterpret_cast<const char*>(recvBuffer_.data()),
            static_cast<long>(received));
        if (inputResult < 0) {
            logging::warn("ikcp_input for session {} failed: {}", session->sessionId, inputResult);
        }
    }

    // Update all KCP instances and collect received application packets
    const uint32_t now = nowMs();
    std::vector<uint32_t> disconnectedSessions;
    for (auto& [id, session] : sessions_) {
        if (!session.kcp) {
            continue;
        }
        ikcp_update(session.kcp, now);
        processReceivedPackets(session);
        if (session.pendingDisconnect) {
            disconnectedSessions.push_back(id);
        }
    }
    for (uint32_t sessionId : disconnectedSessions) {
        logging::info("Session {} disconnected by client", sessionId);
        destroySession(sessionId, true);
    }
    removeTimedOutSessions(now);
}

bool KcpServer::hasSession(uint32_t sessionId) const {
    return sessions_.find(sessionId) != sessions_.end();
}

std::vector<uint32_t> KcpServer::getSessionIds() const {
    std::vector<uint32_t> ids;
    ids.reserve(sessions_.size());
    for (const auto& [id, session] : sessions_) {
        ids.push_back(id);
    }
    return ids;
}

KcpServer::SessionState* KcpServer::findSessionByConv(uint32_t conv) {
    auto it = sessions_.find(conv);  // conv == sessionId
    if (it != sessions_.end()) {
        return &it->second;
    }
    return nullptr;
}

KcpServer::SessionState* KcpServer::findSessionByEndpoint(const Endpoint& endpoint) {
    for (auto& [id, session] : sessions_) {
        if (session.remote == endpoint) {
            return &session;
        }
    }
    return nullptr;
}

void KcpServer::handleHandshake(const Endpoint& sender) {
    // Check if this endpoint already has a session
    SessionState* existing = findSessionByEndpoint(sender);
    if (existing) {
        // Resend handshake response (client may not have received it)
        uint8_t buf[kResponseSize];
        uint32_t magic = kMagic;
        uint32_t conv = existing->sessionId;
        std::memcpy(buf, &magic, 4);
        std::memcpy(buf + 4, &conv, 4);
        sendRawTo(reinterpret_cast<const char*>(buf), sizeof(buf), sender);
        return;
    }

    // Create new session
    auto& session = createSession(sender);

    // Send handshake response with assigned conv
    uint8_t buf[kResponseSize];
    uint32_t magic = kMagic;
    uint32_t conv = session.sessionId;
    std::memcpy(buf, &magic, 4);
    std::memcpy(buf + 4, &conv, 4);
    sendRawTo(reinterpret_cast<const char*>(buf), sizeof(buf), sender);
}

KcpServer::SessionState& KcpServer::createSession(const Endpoint& endpoint) {
    const uint32_t sessionId = nextSessionId_++;

    SessionState session;
    session.sessionId = sessionId;
    session.remote = endpoint;
    session.lastReceiveMs = nowMs();

    // Create KCP output context (must remain stable in memory)
    outputContexts_[sessionId] = KcpOutputContext{this, sessionId};

    // Use sessionId as conv — each session has a unique conv
    session.kcp = ikcp_create(sessionId, &outputContexts_[sessionId]);
    if (!session.kcp) {
        crash("ikcp_create failed for session {}", sessionId);
    }

    ikcp_setoutput(session.kcp, &KcpServer::kcpOutput);
    ikcp_nodelay(session.kcp, 1, 10, 2, 1);
    ikcp_wndsize(session.kcp, 256, 256);
    ikcp_setmtu(session.kcp, 1200);
    session.kcp->rx_minrto = 10;

    auto [it, inserted] = sessions_.emplace(sessionId, std::move(session));

    logging::info("New session {} from {}:{}", sessionId, endpoint.address().to_string(), endpoint.port());

    if (onConnect_) {
        onConnect_(sessionId);
    }

    return it->second;
}

void KcpServer::destroySession(uint32_t sessionId, bool notify) {
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return;
    }

    if (it->second.kcp) {
        ikcp_release(it->second.kcp);
        it->second.kcp = nullptr;
    }
    sessions_.erase(it);
    outputContexts_.erase(sessionId);

    if (notify && onDisconnect_) {
        onDisconnect_(sessionId);
    }
}

void KcpServer::removeTimedOutSessions(uint32_t now) {
    std::vector<uint32_t> timedOutSessions;
    for (const auto& [id, session] : sessions_) {
        if (now - session.lastReceiveMs >= kSessionTimeoutMs) {
            timedOutSessions.push_back(id);
        }
    }

    for (uint32_t sessionId : timedOutSessions) {
        logging::info("Session {} timed out", sessionId);
        destroySession(sessionId, true);
    }
}

void KcpServer::processReceivedPackets(SessionState& session) {
    for (;;) {
        const int packetSize = ikcp_peeksize(session.kcp);
        if (packetSize < 0) {
            break;
        }
        std::vector<uint8_t> packet(static_cast<size_t>(packetSize));
        const int received = ikcp_recv(session.kcp, reinterpret_cast<char*>(packet.data()), static_cast<int>(packet.size()));
        if (received < 0) {
            break;
        }
        packet.resize(static_cast<size_t>(received));

        if (onPacket_ && !onPacket_(session.sessionId, packet)) {
            session.pendingDisconnect = true;
            break;
        }
    }
}

int KcpServer::kcpOutput(const char* buf, int len, ikcpcb* kcp, void* user) {
    auto* ctx = static_cast<KcpOutputContext*>(user);
    auto it = ctx->server->sessions_.find(ctx->sessionId);
    if (it == ctx->server->sessions_.end()) {
        return -1;
    }
    return ctx->server->sendRawTo(buf, static_cast<size_t>(len), it->second.remote);
}

int KcpServer::sendRawTo(const char* data, size_t size, const Endpoint& endpoint) {
    asio::error_code ec;
    socket_.send_to(asio::buffer(data, size), endpoint, 0, ec);
    if (ec) {
        logging::warn("UDP send error: {}", ec.message());
        return -1;
    }
    return 0;
}

uint32_t KcpServer::nowMs() const {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
