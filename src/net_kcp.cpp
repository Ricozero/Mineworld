#include "net_kcp.h"

#include <array>
#include <chrono>
#include <limits>
#include <random>

#include "log.h"

namespace {

constexpr int kRecvMtu = 1400;
constexpr uint32_t kMagic = 0x4B435048;
constexpr uint32_t kProtocolVersion = 1;
constexpr size_t kRequestSize = 16;
constexpr size_t kResponseSize = 20;
constexpr uint32_t kHandshakeRetryMs = 500;
constexpr uint32_t kSessionTimeoutMs = 10'000;
constexpr uint32_t kSessionReplaceGraceMs = 3'000;
constexpr size_t kMaxSessions = 1024;
constexpr uint32_t kHandshakeRateWindowMs = 1000;
constexpr uint32_t kMaxHandshakesPerWindow = 20;
constexpr uint32_t kHandshakeRateRetentionMs = 10'000;
constexpr size_t kMaxHandshakeRateStates = 4096;
constexpr uint32_t kRateLimitCleanupIntervalMs = 1000;

bool isUdpPeerReset(asio::error_code ec) {
    return ec == asio::error::connection_reset;
}

void encodeUint32Be(uint8_t* output, uint32_t value) {
    output[0] = static_cast<uint8_t>(value >> 24);
    output[1] = static_cast<uint8_t>(value >> 16);
    output[2] = static_cast<uint8_t>(value >> 8);
    output[3] = static_cast<uint8_t>(value);
}

uint32_t decodeUint32Be(const uint8_t* input) {
    return (static_cast<uint32_t>(input[0]) << 24) |
           (static_cast<uint32_t>(input[1]) << 16) |
           (static_cast<uint32_t>(input[2]) << 8) |
           static_cast<uint32_t>(input[3]);
}

uint32_t decodeUint32Le(const uint8_t* input) {
    return static_cast<uint32_t>(input[0]) |
           (static_cast<uint32_t>(input[1]) << 8) |
           (static_cast<uint32_t>(input[2]) << 16) |
           (static_cast<uint32_t>(input[3]) << 24);
}

void encodeUint64Be(uint8_t* output, uint64_t value) {
    for (int index = 7; index >= 0; --index) {
        output[index] = static_cast<uint8_t>(value);
        value >>= 8;
    }
}

uint64_t decodeUint64Be(const uint8_t* input) {
    uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8) | input[index];
    }
    return value;
}

uint64_t generateHandshakeNonce() {
    std::random_device randomDevice;
    uint64_t nonce = (static_cast<uint64_t>(randomDevice()) << 32) ^ randomDevice();
    nonce ^= static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    return nonce == 0 ? 1 : nonce;
}

}  // namespace

KcpClient::KcpClient(asio::io_context& ioContext, uint16_t localPort)
    : socket_(ioContext, Udp::endpoint(Udp::v4(), localPort)), recvBuffer_(kRecvMtu) {
    socket_.non_blocking(true);
}

KcpClient::~KcpClient() {
    reset();
}

void KcpClient::connect(const Endpoint& endpoint) {
    reset();
    remote_ = endpoint;
    handshakeNonce_ = generateHandshakeNonce();
    handshakeStarted_ = true;
    sendHandshakeRequest(nowMs());
}

void KcpClient::reset() {
    if (kcp_) {
        ikcp_release(kcp_);
        kcp_ = nullptr;
    }
    remote_.reset();
    recvPackets_.clear();
    handshakeNonce_ = 0;
    lastHandshakeSendMs_ = 0;
    handshakeStarted_ = false;
    versionMismatchLogged_ = false;
}

void KcpClient::sendHandshakeRequest(uint32_t now) {
    if (!remote_ || !handshakeStarted_) {
        return;
    }

    std::array<uint8_t, kRequestSize> buffer{};
    encodeUint32Be(buffer.data(), kMagic);
    encodeUint32Be(buffer.data() + 4, kProtocolVersion);
    encodeUint64Be(buffer.data() + 8, handshakeNonce_);
    lastHandshakeSendMs_ = now;

    asio::error_code error;
    socket_.send_to(asio::buffer(buffer), *remote_, 0, error);
    if (error) {
        logging::warn("Failed to send handshake request: {}", error.message());
    }
}

void KcpClient::initKcp(uint32_t conv) {
    if (kcp_ || conv == 0) {
        return;
    }

    kcp_ = ikcp_create(conv, this);
    if (!kcp_) {
        logging::error("ikcp_create failed for client");
        return;
    }

    ikcp_setoutput(kcp_, &KcpClient::kcpOutput);
    ikcp_nodelay(kcp_, 1, 10, 2, 1);
    ikcp_wndsize(kcp_, 256, 256);
    ikcp_setmtu(kcp_, 1200);
    kcp_->rx_minrto = 10;

    handshakeStarted_ = false;
    logging::info("Handshake complete, conv = {}", conv);
}

void KcpClient::sendReliable(const std::vector<uint8_t>& payload) {
    if (!kcp_ || !remote_) {
        return;
    }
    const int result = ikcp_send(kcp_, reinterpret_cast<const char*>(payload.data()), static_cast<int>(payload.size()));
    if (result < 0) {
        logging::warn("ikcp_send failed: {}", result);
    }
}

void KcpClient::flush() {
    if (!kcp_) {
        return;
    }
    ikcp_update(kcp_, nowMs());
    ikcp_flush(kcp_);
}

void KcpClient::pump() {
    for (;;) {
        Udp::endpoint sender;
        asio::error_code error;
        const auto received = socket_.receive_from(asio::buffer(recvBuffer_), sender, 0, error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            break;
        }
        if (isUdpPeerReset(error)) {
            break;
        }
        if (error) {
            logging::warn("UDP receive error: {}", error.message());
            break;
        }
        if (!remote_ || *remote_ != sender) {
            continue;
        }

        if (received == kResponseSize && decodeUint32Be(recvBuffer_.data()) == kMagic) {
            const uint32_t version = decodeUint32Be(recvBuffer_.data() + 4);
            const uint64_t nonce = decodeUint64Be(recvBuffer_.data() + 8);
            const uint32_t conv = decodeUint32Be(recvBuffer_.data() + 16);
            if (version != kProtocolVersion) {
                if (!versionMismatchLogged_) {
                    versionMismatchLogged_ = true;
                    logging::error("Server protocol version {} does not match client version {}", version, kProtocolVersion);
                }
            } else if (nonce == handshakeNonce_ && conv != 0) {
                initKcp(conv);
            }
            continue;
        }

        if (!kcp_) {
            continue;
        }

        const int inputResult = ikcp_input(kcp_, reinterpret_cast<const char*>(recvBuffer_.data()), static_cast<long>(received));
        if (inputResult < 0) {
            logging::warn("ikcp_input failed: {}", inputResult);
        }
    }

    const uint32_t now = nowMs();
    if (!kcp_) {
        if (handshakeStarted_ && now - lastHandshakeSendMs_ >= kHandshakeRetryMs) {
            sendHandshakeRequest(now);
        }
        return;
    }

    ikcp_update(kcp_, now);

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

bool KcpClient::popPacket(std::vector<uint8_t>& outPacket) {
    if (recvPackets_.empty()) {
        return false;
    }
    outPacket = std::move(recvPackets_.front());
    recvPackets_.pop_front();
    return true;
}

int KcpClient::kcpOutput(const char* buffer, int length, ikcpcb*, void* user) {
    auto* self = static_cast<KcpClient*>(user);
    return self->sendRaw(buffer, static_cast<size_t>(length));
}

int KcpClient::sendRaw(const char* data, size_t size) {
    if (!remote_) {
        return -1;
    }
    asio::error_code error;
    socket_.send_to(asio::buffer(data, size), *remote_, 0, error);
    if (error) {
        logging::warn("UDP send error: {}", error.message());
        return -1;
    }
    return 0;
}

uint32_t KcpClient::nowMs() const {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

KcpServer::KcpServer(asio::io_context& ioContext, uint16_t localPort)
    : socket_(ioContext, Udp::endpoint(Udp::v4(), localPort)), recvBuffer_(kRecvMtu) {
    socket_.non_blocking(true);
}

KcpServer::~KcpServer() {
    for (auto& [sessionId, session] : sessions_) {
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
    if (it == sessions_.end() || !it->second.kcp) {
        return;
    }
    const int result = ikcp_send(it->second.kcp, reinterpret_cast<const char*>(payload.data()), static_cast<int>(payload.size()));
    if (result < 0) {
        logging::warn("ikcp_send to session {} failed: {}", sessionId, result);
    }
}

void KcpServer::pump() {
    for (;;) {
        Udp::endpoint sender;
        asio::error_code error;
        const auto received = socket_.receive_from(asio::buffer(recvBuffer_), sender, 0, error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            break;
        }
        if (isUdpPeerReset(error)) {
            break;
        }
        if (error) {
            logging::warn("UDP receive error: {}", error.message());
            break;
        }

        const uint32_t receiveTime = nowMs();
        if (received == kRequestSize && decodeUint32Be(recvBuffer_.data()) == kMagic) {
            const uint32_t version = decodeUint32Be(recvBuffer_.data() + 4);
            const uint64_t nonce = decodeUint64Be(recvBuffer_.data() + 8);
            if (version == kProtocolVersion && nonce != 0 && allowHandshake(sender, receiveTime)) {
                handleHandshake(sender, nonce, receiveTime);
            }
            continue;
        }

        if (received < 4) {
            continue;
        }
        const uint32_t conv = decodeUint32Le(recvBuffer_.data());
        SessionState* session = findSessionByConv(conv);
        if (!session) {
            continue;
        }
        if (session->remote != sender) {
            logging::warn("Rejected packet for session {} from unexpected endpoint {}:{}", session->sessionId, sender.address().to_string(), sender.port());
            continue;
        }

        const int inputResult = ikcp_input(
            session->kcp,
            reinterpret_cast<const char*>(recvBuffer_.data()),
            static_cast<long>(received));
        if (inputResult < 0) {
            logging::warn("ikcp_input for session {} failed: {}", session->sessionId, inputResult);
            continue;
        }
        session->lastReceiveMs = receiveTime;
    }

    const uint32_t now = nowMs();
    std::vector<uint32_t> disconnectedSessions;
    for (auto& [sessionId, session] : sessions_) {
        if (!session.kcp) {
            continue;
        }
        ikcp_update(session.kcp, now);
        processReceivedPackets(session);
        if (session.pendingDisconnect) {
            disconnectedSessions.push_back(sessionId);
        }
    }
    for (uint32_t sessionId : disconnectedSessions) {
        logging::info("Session {} disconnected by client", sessionId);
        destroySession(sessionId, true);
    }
    removeTimedOutSessions(now);
    cleanupHandshakeRateLimits(now);
}

bool KcpServer::hasSession(uint32_t sessionId) const {
    return sessions_.find(sessionId) != sessions_.end();
}

std::vector<uint32_t> KcpServer::getSessionIds() const {
    std::vector<uint32_t> sessionIds;
    sessionIds.reserve(sessions_.size());
    for (const auto& [sessionId, session] : sessions_) {
        sessionIds.push_back(sessionId);
    }
    return sessionIds;
}

std::optional<uint32_t> KcpServer::allocateSessionId() {
    if (sessions_.size() >= kMaxSessions) {
        return std::nullopt;
    }

    const uint32_t firstCandidate = nextSessionId_ == 0 ? 1 : nextSessionId_;
    uint32_t candidate = firstCandidate;
    do {
        nextSessionId_ = candidate == std::numeric_limits<uint32_t>::max() ? 1 : candidate + 1;
        if (sessions_.find(candidate) == sessions_.end()) {
            return candidate;
        }
        candidate = nextSessionId_;
    } while (candidate != firstCandidate);

    return std::nullopt;
}

KcpServer::SessionState* KcpServer::findSessionByConv(uint32_t conv) {
    auto it = sessions_.find(conv);
    return it == sessions_.end() ? nullptr : &it->second;
}

KcpServer::SessionState* KcpServer::findSessionByEndpoint(const Endpoint& endpoint) {
    auto endpointIt = endpointSessions_.find(endpoint);
    if (endpointIt == endpointSessions_.end()) {
        return nullptr;
    }
    auto sessionIt = sessions_.find(endpointIt->second);
    if (sessionIt == sessions_.end()) {
        endpointSessions_.erase(endpointIt);
        return nullptr;
    }
    return &sessionIt->second;
}

KcpServer::SessionState* KcpServer::createSession(const Endpoint& endpoint, uint64_t handshakeNonce) {
    const std::optional<uint32_t> sessionId = allocateSessionId();
    if (!sessionId) {
        logging::warn("Rejected connection from {}:{}: session limit reached",
                      endpoint.address().to_string(), endpoint.port());
        return nullptr;
    }

    auto [it, inserted] = sessions_.try_emplace(*sessionId);
    if (!inserted) {
        return nullptr;
    }

    SessionState& session = it->second;
    session.sessionId = *sessionId;
    session.remote = endpoint;
    session.handshakeNonce = handshakeNonce;
    session.lastReceiveMs = nowMs();
    session.outputContext = KcpOutputContext{this, *sessionId};
    session.kcp = ikcp_create(*sessionId, &session.outputContext);
    if (!session.kcp) {
        logging::error("ikcp_create failed for session {}", *sessionId);
        sessions_.erase(it);
        return nullptr;
    }

    ikcp_setoutput(session.kcp, &KcpServer::kcpOutput);
    ikcp_nodelay(session.kcp, 1, 10, 2, 1);
    ikcp_wndsize(session.kcp, 256, 256);
    ikcp_setmtu(session.kcp, 1200);
    session.kcp->rx_minrto = 10;

    endpointSessions_[endpoint] = *sessionId;
    logging::info("New session {} from {}:{}", *sessionId, endpoint.address().to_string(), endpoint.port());
    if (onConnect_) {
        onConnect_(*sessionId);
    }
    return &session;
}

void KcpServer::handleHandshake(const Endpoint& sender, uint64_t handshakeNonce, uint32_t now) {
    SessionState* existing = findSessionByEndpoint(sender);
    if (existing && existing->handshakeNonce == handshakeNonce) {
        existing->lastReceiveMs = now;
        sendHandshakeResponse(*existing);
        return;
    }
    if (existing) {
        if (now - existing->lastReceiveMs < kSessionReplaceGraceMs) {
            logging::warn("Ignored conflicting handshake for live session {} from {}:{}", existing->sessionId, sender.address().to_string(), sender.port());
            return;
        }
        logging::info("Replacing stale session {} after reconnect from {}:{}", existing->sessionId, sender.address().to_string(), sender.port());
        destroySession(existing->sessionId, true);
    }

    SessionState* session = createSession(sender, handshakeNonce);
    if (session) {
        sendHandshakeResponse(*session);
    }
}

void KcpServer::sendHandshakeResponse(const SessionState& session) {
    std::array<uint8_t, kResponseSize> buffer{};
    encodeUint32Be(buffer.data(), kMagic);
    encodeUint32Be(buffer.data() + 4, kProtocolVersion);
    encodeUint64Be(buffer.data() + 8, session.handshakeNonce);
    encodeUint32Be(buffer.data() + 16, session.sessionId);
    sendRawTo(reinterpret_cast<const char*>(buffer.data()), buffer.size(), session.remote);
}

void KcpServer::destroySession(uint32_t sessionId, bool notify) {
    auto sessionIt = sessions_.find(sessionId);
    if (sessionIt == sessions_.end()) {
        return;
    }

    auto endpointIt = endpointSessions_.find(sessionIt->second.remote);
    if (endpointIt != endpointSessions_.end() && endpointIt->second == sessionId) {
        endpointSessions_.erase(endpointIt);
    }
    if (sessionIt->second.kcp) {
        ikcp_release(sessionIt->second.kcp);
        sessionIt->second.kcp = nullptr;
    }
    sessions_.erase(sessionIt);

    if (notify && onDisconnect_) {
        onDisconnect_(sessionId);
    }
}

void KcpServer::removeTimedOutSessions(uint32_t now) {
    std::vector<uint32_t> timedOutSessions;
    for (const auto& [sessionId, session] : sessions_) {
        if (now - session.lastReceiveMs >= kSessionTimeoutMs) {
            timedOutSessions.push_back(sessionId);
        }
    }

    for (uint32_t sessionId : timedOutSessions) {
        logging::info("Session {} timed out", sessionId);
        destroySession(sessionId, true);
    }
}

bool KcpServer::allowHandshake(const Endpoint& sender, uint32_t now) {
    auto it = handshakeRateStates_.find(sender.address());
    if (it == handshakeRateStates_.end()) {
        if (handshakeRateStates_.size() >= kMaxHandshakeRateStates) {
            evictStalestHandshakeRateState();
        }
        it = handshakeRateStates_.emplace(sender.address(), HandshakeRateState{now, 0, now}).first;
    }

    HandshakeRateState& state = it->second;
    if (now - state.windowStartMs >= kHandshakeRateWindowMs) {
        state.windowStartMs = now;
        state.attempts = 0;
    }
    if (state.attempts >= kMaxHandshakesPerWindow) {
        return false;
    }
    state.lastSeenMs = now;
    ++state.attempts;
    return true;
}

void KcpServer::evictStalestHandshakeRateState() {
    auto stalest = handshakeRateStates_.begin();
    for (auto it = handshakeRateStates_.begin(); it != handshakeRateStates_.end(); ++it) {
        if (it->second.lastSeenMs < stalest->second.lastSeenMs) {
            stalest = it;
        }
    }
    if (stalest != handshakeRateStates_.end()) {
        handshakeRateStates_.erase(stalest);
    }
}

void KcpServer::cleanupHandshakeRateLimits(uint32_t now) {
    if (now - lastRateLimitCleanupMs_ < kRateLimitCleanupIntervalMs) {
        return;
    }
    lastRateLimitCleanupMs_ = now;

    for (auto it = handshakeRateStates_.begin(); it != handshakeRateStates_.end();) {
        if (now - it->second.lastSeenMs >= kHandshakeRateRetentionMs) {
            it = handshakeRateStates_.erase(it);
        } else {
            ++it;
        }
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

int KcpServer::kcpOutput(const char* buffer, int length, ikcpcb*, void* user) {
    auto* context = static_cast<KcpOutputContext*>(user);
    auto sessionIt = context->server->sessions_.find(context->sessionId);
    if (sessionIt == context->server->sessions_.end()) {
        return -1;
    }
    return context->server->sendRawTo(buffer, static_cast<size_t>(length), sessionIt->second.remote);
}

int KcpServer::sendRawTo(const char* data, size_t size, const Endpoint& endpoint) {
    asio::error_code error;
    socket_.send_to(asio::buffer(data, size), endpoint, 0, error);
    if (error) {
        logging::warn("UDP send error: {}", error.message());
        return -1;
    }
    return 0;
}

uint32_t KcpServer::nowMs() const {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
