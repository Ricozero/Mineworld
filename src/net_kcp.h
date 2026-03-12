#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <asio.hpp>
#include <ikcp.h>

class KcpChannel {
public:
    using Udp = asio::ip::udp;
    using Endpoint = Udp::endpoint;

    KcpChannel(asio::io_context& ioContext, uint16_t localPort, uint32_t conv);
    ~KcpChannel();

    void setRemote(const Endpoint& endpoint);
    bool hasRemote() const { return remote_.has_value(); }

    void sendReliable(const std::vector<uint8_t>& payload);
    void pump();
    bool popPacket(std::vector<uint8_t>& outPacket);

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
