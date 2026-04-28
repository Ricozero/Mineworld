#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <asio.hpp>
#include <ikcp.h>

#include "net_channel.h"

class KcpChannel : public IPacketChannel {
public:
    using Udp = asio::ip::udp;

    KcpChannel(asio::io_context& ioContext, uint16_t localPort, uint32_t conv);
    ~KcpChannel() override;

    void setRemote(const Endpoint& endpoint) override;
    bool hasRemote() const override { return remote_.has_value(); }

    void sendReliable(const std::vector<uint8_t>& payload) override;
    void pump() override;
    bool popPacket(std::vector<uint8_t>& outPacket) override;

private:
    static int kcpOutput(const char* buf, int len, ikcpcb* kcp, void* user);
    int sendRaw(const char* data, size_t size);
    uint32_t nowMs() const;

    asio::io_context& ioContext_;
    Udp::socket socket_;
    ikcpcb* kcp_ = nullptr;

    std::optional<Endpoint> remote_;
    std::vector<uint8_t> recvBuffer_;
    std::vector<std::vector<uint8_t>> recvPackets_;
};
