#include "net_kcp.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>

#include "helper.h"

namespace {

constexpr int kRecvMtu = 1400;

}  // namespace

KcpChannel::KcpChannel(asio::io_context& ioContext, uint16_t localPort, uint32_t conv)
    : ioContext_(ioContext), socket_(ioContext, Udp::endpoint(Udp::v4(), localPort)), recvBuffer_(kRecvMtu) {
    socket_.non_blocking(true);

    kcp_ = ikcp_create(conv, this);
    if (!kcp_) {
        crash("ikcp_create failed");
    }

    ikcp_setoutput(kcp_, &KcpChannel::kcpOutput);
    ikcp_nodelay(kcp_, 1, 10, 2, 1);
    ikcp_wndsize(kcp_, 256, 256);
    ikcp_setmtu(kcp_, 1200);
    kcp_->rx_minrto = 10;
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

void KcpChannel::sendReliable(const std::vector<uint8_t>& payload) {
    if (!kcp_ || !remote_) {
        return;
    }
    const int result = ikcp_send(kcp_, reinterpret_cast<const char*>(payload.data()), static_cast<int>(payload.size()));
    if (result < 0) {
        spdlog::warn("ikcp_send failed: {}", result);
    }
}

void KcpChannel::pump() {
    if (!kcp_) {
        return;
    }

    for (;;) {
        Udp::endpoint sender;
        asio::error_code ec;
        const auto received = socket_.receive_from(asio::buffer(recvBuffer_), sender, 0, ec);
        if (ec == asio::error::would_block || ec == asio::error::try_again) {
            break;
        }
        if (ec) {
            spdlog::warn("UDP receive error: {}", ec.message());
            break;
        }
        if (!remote_) {
            remote_ = sender;
        }
        if (*remote_ != sender) {
            continue;
        }

        const int inputResult = ikcp_input(kcp_, reinterpret_cast<const char*>(recvBuffer_.data()), static_cast<long>(received));
        if (inputResult < 0) {
            spdlog::warn("ikcp_input failed: {}", inputResult);
        }
    }

    ikcp_update(kcp_, nowMs());

    for (;;) {
        std::vector<uint8_t> packet(8192);
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
        spdlog::warn("UDP send error: {}", ec.message());
        return -1;
    }
    return 0;
}

uint32_t KcpChannel::nowMs() const {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
