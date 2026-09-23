#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include "net_kcp.h"
#include "test_support.h"

namespace {

using namespace test_support;

TEST(KcpTransportTest, LoopbackOrderingCloseReconnectAndTimeout) {
    ensureLogger();
    asio::io_context io;
    asio::ip::udp::socket reservation(io, asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
    const auto port = reservation.local_endpoint().port();
    reservation.close();
    KcpServer server(io, port);
    KcpClient client(io, 0);
    const INetClient::Endpoint endpoint(asio::ip::make_address("127.0.0.1"), port);
    const std::vector<uint8_t> packet(32 * 1024, 42);
    ASSERT_FALSE(client.send(packet)) << "disconnected send succeeded";
    ASSERT_FALSE(server.send(999, packet)) << "disconnected send succeeded";
    client.connect(endpoint);
    ASSERT_NO_FATAL_FAILURE(pumpUntil([&] { server.pump(); client.pump(); }, [&] { return client.isConnected(); }));
    NetEvent event;
    ASSERT_TRUE(server.popEvent(event)) << "server Connected event missing";
    ASSERT_EQ(event.type, NetEventType::Connected) << "server Connected event missing";
    const uint32_t sessionId = event.sessionId;
    ASSERT_TRUE(client.popEvent(event)) << "client Connected event missing";
    ASSERT_EQ(event.type, NetEventType::Connected) << "client Connected event missing";
    ASSERT_EQ(event.sessionId, sessionId) << "client Connected event missing";
    ASSERT_FALSE(client.send(std::vector<uint8_t>(300 * 1024))) << "oversized KCP message accepted";
    ASSERT_FALSE(server.send(sessionId, std::vector<uint8_t>(300 * 1024))) << "oversized KCP message accepted";
    ASSERT_TRUE(client.send(packet)) << "client send failed";
    client.flush();
    bool received = false;
    ASSERT_NO_FATAL_FAILURE(pumpUntil(
        [&] {
            server.pump();
            client.pump();
            if (server.popEvent(event)) {
                ASSERT_EQ(event.type, NetEventType::Packet) << "fragmented message changed";
                ASSERT_EQ(event.payload, packet) << "fragmented message changed";
                received = true;
            }
        },
        [&] { return received; }));
    for (uint8_t i = 1; i <= 3; ++i) {
        ASSERT_TRUE((server.send(sessionId, std::vector<uint8_t>{i}))) << "server send failed";
    }
    server.flush();
    int next = 1;
    ASSERT_NO_FATAL_FAILURE(pumpUntil(
        [&] {
            server.pump();
            client.pump();
            while (client.popEvent(event)) {
                ASSERT_EQ(event.type, NetEventType::Packet) << "server FIFO violated";
                ASSERT_EQ(event.payload, (std::vector<uint8_t>{static_cast<uint8_t>(next++)})) << "server FIFO violated";
            }
        },
        [&] { return next == 4; }));
    for (uint8_t i = 1; i <= 3; ++i) {
        ASSERT_TRUE((client.send(std::vector<uint8_t>{i}))) << "close test send failed";
    }
    client.flush();
    received = false;
    ASSERT_NO_FATAL_FAILURE(pumpUntil(
        [&] {
            server.pump();
            client.pump();
            if (server.popEvent(event)) {
                ASSERT_EQ(event.type, NetEventType::Packet) << "first packet missing before close";
                ASSERT_EQ(event.payload.size(), 1u);
                ASSERT_EQ(event.payload[0], 1) << "first packet missing before close";
                server.close(sessionId);
                received = true;
            }
        },
        [&] { return received; }));
    ASSERT_TRUE(server.popEvent(event)) << "close did not cancel subsequent packets";
    ASSERT_EQ(event.type, NetEventType::Disconnected) << "close did not cancel subsequent packets";
    ASSERT_FALSE(server.popEvent(event)) << "closed session still has work";
    ASSERT_FALSE(server.hasSession(sessionId)) << "closed session still has work";
    server.close(sessionId);
    ASSERT_FALSE(server.popEvent(event)) << "duplicate disconnect event";
    client.close();
    client.connect(endpoint);
    ASSERT_NO_FATAL_FAILURE(pumpUntil([&] { server.pump(); client.pump(); }, [&] { return client.isConnected(); }));
    ASSERT_TRUE(client.popEvent(event)) << "reconnect kept old events";
    ASSERT_EQ(event.type, NetEventType::Connected) << "reconnect kept old events";
    ASSERT_NE(event.sessionId, sessionId) << "reconnect kept old events";
    ASSERT_TRUE(server.popEvent(event)) << "reconnected server event missing";
    ASSERT_EQ(event.type, NetEventType::Connected) << "reconnected server event missing";
    const uint32_t nextSessionId = event.sessionId;
    ASSERT_TRUE((server.send(nextSessionId, std::vector<uint8_t>{77}))) << "pre-timeout send failed";
    server.flush();
    ASSERT_NO_FATAL_FAILURE(pumpUntil([&] { server.pump(); client.pump(); }, [&] { return !client.isConnected() && !server.hasSession(nextSessionId); }, std::chrono::seconds(12)));
    ASSERT_TRUE(client.popEvent(event)) << "timeout discarded an earlier packet";
    ASSERT_EQ(event.type, NetEventType::Packet) << "timeout discarded an earlier packet";
    ASSERT_EQ(event.payload, (std::vector<uint8_t>{77})) << "timeout discarded an earlier packet";
    ASSERT_TRUE(client.popEvent(event)) << "client timeout event missing";
    ASSERT_EQ(event.type, NetEventType::Disconnected) << "client timeout event missing";
    ASSERT_TRUE(server.popEvent(event)) << "server timeout event missing";
    ASSERT_EQ(event.type, NetEventType::Disconnected) << "server timeout event missing";
    client.connect(endpoint);
    ASSERT_NO_FATAL_FAILURE(pumpUntil([&] { server.pump(); client.pump(); }, [&] { return client.isConnected(); }));
    client.close();
    client.close();
    ASSERT_TRUE(client.popEvent(event)) << "client close was not idempotent";
    ASSERT_EQ(event.type, NetEventType::Disconnected) << "client close was not idempotent";
    ASSERT_FALSE(client.popEvent(event)) << "client close was not idempotent";
}

}  // namespace
