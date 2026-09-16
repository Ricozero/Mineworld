#include <gtest/gtest.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_set>

#include "chunk.h"
#include "config.h"
#include "game_server.h"
#include "net_kcp.h"
#include "net_protocol.h"

namespace {

using Payload = mineworld::net::NetMessagePayload;
using Clock = std::chrono::steady_clock;

void ensureLogger() {
    if (!spdlog::get("App")) {
        spdlog::register_logger(std::make_shared<spdlog::logger>("App", std::make_shared<spdlog::sinks::null_sink_mt>()));
    }
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void sameBlocks(const ChunkData& a, const ChunkData& b) {
    for (size_t i = 0; i < ChunkLayout::BLOCK_COUNT; ++i) {
        require(a.get(i) == b.get(i), "chunk contents differ");
    }
}

ChunkData patternedData(bool noise) {
    ChunkData data;
    std::mt19937 random(42);
    for (size_t i = 0; i < ChunkLayout::BLOCK_COUNT; ++i) {
        const auto type = noise ? random() % 8 : (i / 256) % 3;
        const auto orientation = noise ? random() % 6 : 0;
        data.set(i, BlockData{static_cast<BlockType>(type), static_cast<BlockOrientation>(orientation)});
    }
    return data;
}

TEST(ChunkCodecTest, RoundTripCorruptionAndCacheInvalidation) {
    for (const auto& data : {ChunkData{}, ChunkData{BlockType::Stone}, patternedData(false), patternedData(true)}) {
        const auto encoded = ChunkCodec::encode(data);
        ChunkData decoded;
        require(ChunkCodec::decode(encoded.compression, encoded.uncompressedSize, encoded.bytes, decoded), "codec round trip failed");
        sameBlocks(data, decoded);
        if (data.isUniform()) {
            require(encoded.compression == ChunkCompression::None && encoded.bytes.size() == 3, "uniform data should remain raw");
        }
        require(!ChunkCodec::decode(encoded.compression, encoded.uncompressedSize + 1, encoded.bytes, decoded), "accepted wrong decoded length");
        require(!ChunkCodec::decode(static_cast<ChunkCompression>(255), encoded.uncompressedSize, encoded.bytes, decoded), "accepted unknown compression");
        require(!ChunkCodec::decode(encoded.compression, ChunkData::MAX_SERIALIZED_SIZE + 1, encoded.bytes, decoded), "accepted oversized decoded length");
        sameBlocks(data, decoded);
    }
    require(ChunkCodec::encode(patternedData(false)).compression == ChunkCompression::Lz4, "pattern did not compress");
    require(ChunkCodec::encode(patternedData(true)).compression == ChunkCompression::None, "incompressible data did not stay raw");
    ChunkData unchanged{BlockType::Stone};
    const std::vector<uint8_t> invalid{0xff};
    require(!ChunkCodec::decode(ChunkCompression::Lz4, 100, invalid, unchanged), "accepted corrupt LZ4");
    require(unchanged.isUniform() && unchanged.uniformBlock().type == BlockType::Stone, "failed decode modified output");
    const std::vector<uint8_t> invalidBlock{0, 255, 255};
    require(!ChunkCodec::decode(ChunkCompression::None, 3, invalidBlock, unchanged), "accepted invalid block");

    Chunk chunk({0, 0, 0});
    require(chunk.applyData(1, patternedData(false)), "initial apply failed");
    const auto first = chunk.getEncodedSnapshot();
    require(first == chunk.getEncodedSnapshot(), "cache not shared");
    chunk.setBlock({0, 0, 0}, chunk.getBlock({0, 0, 0}));
    require(first == chunk.getEncodedSnapshot(), "no-op invalidated cache");
    chunk.setBlock({0, 0, 0}, BlockData{BlockType::Water});
    const auto changed = chunk.getEncodedSnapshot();
    require(changed != first && changed->revision == 2 && first->revision == 1, "mutation cache mismatch");
    require(!chunk.applyData(1, ChunkData{}), "accepted stale revision");
    require(changed == chunk.getEncodedSnapshot(), "rejected apply invalidated cache");
    require(chunk.applyData(2, ChunkData{BlockType::Sand}), "equal revision apply failed");
    require(changed != chunk.getEncodedSnapshot(), "equal revision did not invalidate cache");
    chunk.clearBlock({0, 0, 0});
    require(chunk.getEncodedSnapshot()->revision == 3, "clearBlock did not invalidate cache");
    Chunk reloaded({0, 0, 0});
    reloaded.applyData(1, ChunkData{BlockType::Water});
    require(first != reloaded.getEncodedSnapshot(), "reloaded chunk reused old lifetime snapshot");
    ChunkData oldDecoded;
    require(ChunkCodec::decode(first->data.compression, first->data.uncompressedSize, first->data.bytes, oldDecoded), "retained snapshot corrupted");
    sameBlocks(oldDecoded, patternedData(false));
}

std::vector<uint8_t> malformedBatch(uint8_t compression, uint32_t rawSize) {
    flatbuffers::FlatBufferBuilder builder;
    const auto valid = ChunkCodec::encode(ChunkData{});
    const mineworld::net::IVec3 pos{0, 0, 0};
    const auto blocks = builder.CreateVector(valid.bytes);
    const auto first = mineworld::net::CreateChunkUpsert(builder, &pos, 1, 0, 3, blocks);
    const auto last = mineworld::net::CreateChunkUpsert(builder, &pos, 2, compression, rawSize, blocks);
    const std::vector entries{first, last};
    const auto batch = mineworld::net::CreateChunkUpsertBatch(builder, builder.CreateVector(entries));
    const auto root = mineworld::net::CreateNetMessage(builder, Payload::ChunkUpsertBatch, batch.Union());
    mineworld::net::FinishNetMessageBuffer(builder, root);
    return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

TEST(ChunkProtocolTest, BatchRoundTripAndLimits) {
    flatbuffers::FlatBufferBuilder builder;
    Chunk chunk({0, 0, 0});
    chunk.applyData(9, patternedData(false));
    std::vector<NetChunkUpsert> entries;
    for (int i = 0; i < 64; ++i) {
        entries.push_back(NetChunkUpsert{{i, 0, 0}, chunk.getEncodedSnapshot()});
    }
    entries.front().snapshot = Chunk({0, 0, 0}).getEncodedSnapshot();
    auto bytes = serializeChunkUpsertBatch(entries, builder);
    require(!bytes.empty() && bytes.size() <= MAX_CHUNK_BATCH_BYTES, "64-entry batch failed");
    std::vector<NetDecodedChunkUpsert> decoded;
    require(deserializeChunkUpsertBatch(bytes, decoded) && decoded.size() == 64, "batch round trip failed");
    sameBlocks(decoded.back().blocks, chunk.getData());
    require(decoded.back().chunkPos.x == 63 && decoded.back().revision == 9, "batch metadata changed");
    require(!deserializeChunkUpsertBatch(malformedBatch(255, 3), decoded) && decoded.size() == 64, "partially published unknown-codec batch");
    require(!deserializeChunkUpsertBatch(malformedBatch(0, 4), decoded) && decoded.size() == 64, "partially published invalid-length batch");
    require(!deserializeChunkUpsertBatch(std::span(bytes).first(bytes.size() / 2), decoded), "accepted truncated FlatBuffer");
    entries.push_back(entries.back());
    require(serializeChunkUpsertBatch(entries, builder).empty(), "accepted 65-entry batch");
    require(serializeChunkUpsertBatch({}, builder).empty(), "accepted empty batch");

    auto raw = std::make_shared<EncodedChunkSnapshot>();
    raw->revision = 1;
    const auto noise = patternedData(true);
    noise.serialize(raw->data.bytes);
    raw->data.uncompressedSize = static_cast<uint32_t>(raw->data.bytes.size());
    entries.assign(64, NetChunkUpsert{{0, 0, 0}, raw});
    require(serializeChunkUpsertBatch(entries, builder).empty(), "accepted batch over 32 KiB");
    while ((bytes = serializeChunkUpsertBatch(entries, builder)).empty()) {
        entries.pop_back();
    }
    require(bytes.size() <= MAX_CHUNK_BATCH_BYTES && deserializeChunkUpsertBatch(bytes, decoded), "byte-boundary batch failed");
    entries.push_back(entries.back());
    require(serializeChunkUpsertBatch(entries, builder).empty(), "boundary plus one was accepted");

    std::vector<NetChunkUnload> unloads(256, NetChunkUnload{{0, 0, 0}, 9});
    std::vector<NetChunkUnload> unloadResult;
    bytes = serializeChunkUnloadBatch(unloads, builder);
    require(deserializeChunkUnloadBatch(bytes, unloadResult) && unloadResult.size() == 256, "unload round trip failed");
    require(unloadResult[0].revision == 9, "unload revision changed");
    unloads.push_back(unloads.back());
    require(serializeChunkUnloadBatch(unloads, builder).empty(), "accepted 257 unloads");
    require(!deserializeChunkUpsertBatch(bytes, decoded), "accepted unload as upsert");
    unloads.resize(1);
    unloads[0].chunkPos.y = 99;
    require(serializeChunkUnloadBatch(unloads, builder).empty(), "accepted out-of-world chunk");
}

template <typename Step, typename Done>
void pumpUntil(Step step, Done done, std::chrono::seconds timeout = std::chrono::seconds(3)) {
    const auto deadline = Clock::now() + timeout;
    while (!done() && Clock::now() < deadline) {
        step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(done(), "loopback transport timed out");
}

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
    require(!client.send(packet) && !server.send(999, packet), "disconnected send succeeded");
    client.connect(endpoint);
    pumpUntil([&] { server.pump(); client.pump(); }, [&] { return client.isConnected(); });
    NetEvent event;
    require(server.popEvent(event) && event.type == NetEventType::Connected, "server Connected event missing");
    const uint32_t sessionId = event.sessionId;
    require(client.popEvent(event) && event.type == NetEventType::Connected && event.sessionId == sessionId, "client Connected event missing");
    require(!client.send(std::vector<uint8_t>(300 * 1024)) && !server.send(sessionId, std::vector<uint8_t>(300 * 1024)), "oversized KCP message accepted");
    require(client.send(packet), "client send failed");
    client.flush();
    bool received = false;
    pumpUntil(
        [&] {
            server.pump();
            client.pump();
            if (server.popEvent(event)) {
                require(event.type == NetEventType::Packet && event.payload == packet, "fragmented message changed");
                received = true;
            }
        },
        [&] { return received; });
    for (uint8_t i = 1; i <= 3; ++i) {
        require(server.send(sessionId, std::vector<uint8_t>{i}), "server send failed");
    }
    server.flush();
    int next = 1;
    pumpUntil(
        [&] {
            server.pump();
            client.pump();
            while (client.popEvent(event)) {
                require(event.type == NetEventType::Packet && event.payload == std::vector<uint8_t>{static_cast<uint8_t>(next++)}, "server FIFO violated");
            }
        },
        [&] { return next == 4; });
    for (uint8_t i = 1; i <= 3; ++i) {
        require(client.send(std::vector<uint8_t>{i}), "close test send failed");
    }
    client.flush();
    received = false;
    pumpUntil(
        [&] {
            server.pump();
            client.pump();
            if (server.popEvent(event)) {
                require(event.type == NetEventType::Packet && event.payload[0] == 1, "first packet missing before close");
                server.close(sessionId);
                received = true;
            }
        },
        [&] { return received; });
    require(server.popEvent(event) && event.type == NetEventType::Disconnected, "close did not cancel subsequent packets");
    require(!server.popEvent(event) && !server.hasSession(sessionId), "closed session still has work");
    server.close(sessionId);
    require(!server.popEvent(event), "duplicate disconnect event");
    client.close();
    client.connect(endpoint);
    pumpUntil([&] { server.pump(); client.pump(); }, [&] { return client.isConnected(); });
    require(client.popEvent(event) && event.type == NetEventType::Connected && event.sessionId != sessionId, "reconnect kept old events");
    require(server.popEvent(event) && event.type == NetEventType::Connected, "reconnected server event missing");
    const uint32_t nextSessionId = event.sessionId;
    require(server.send(nextSessionId, std::vector<uint8_t>{77}), "pre-timeout send failed");
    server.flush();
    pumpUntil([&] { server.pump(); client.pump(); }, [&] { return !client.isConnected() && !server.hasSession(nextSessionId); }, std::chrono::seconds(12));
    require(client.popEvent(event) && event.type == NetEventType::Packet && event.payload == std::vector<uint8_t>{77}, "timeout discarded an earlier packet");
    require(client.popEvent(event) && event.type == NetEventType::Disconnected, "client timeout event missing");
    require(server.popEvent(event) && event.type == NetEventType::Disconnected, "server timeout event missing");
    client.connect(endpoint);
    pumpUntil([&] { server.pump(); client.pump(); }, [&] { return client.isConnected(); });
    client.close();
    client.close();
    require(client.popEvent(event) && event.type == NetEventType::Disconnected && !client.popEvent(event), "client close was not idempotent");
}

class FakeServer final : public INetServer {
public:
    void connect(uint32_t id) {
        sessions.insert(id);
        events.push_back(NetEvent{NetEventType::Connected, id, {}});
        receive(id, serializeClientHello());
    }
    void receive(uint32_t id, std::vector<uint8_t> bytes) {
        events.push_back(NetEvent{NetEventType::Packet, id, std::move(bytes)});
    }
    bool send(uint32_t id, std::span<const uint8_t> bytes) override {
        if (!sessions.contains(id) || getPacketType(bytes) == rejectedType) {
            lastRejected.assign(bytes.begin(), bytes.end());
            return false;
        }
        sent.push_back(NetEvent{NetEventType::Packet, id, {bytes.begin(), bytes.end()}});
        return true;
    }
    void flush() override {}
    void pump() override {}
    bool popEvent(NetEvent& event) override {
        if (events.empty()) return false;
        event = std::move(events.front());
        events.pop_front();
        return true;
    }
    void close(uint32_t id) override {
        if (!sessions.erase(id)) return;
        std::erase_if(events, [id](const NetEvent& event) { return event.sessionId == id; });
        events.push_back(NetEvent{NetEventType::Disconnected, id, {}});
    }
    bool hasSession(uint32_t id) const override { return sessions.contains(id); }

    Payload rejectedType = Payload::NONE;
    std::vector<uint8_t> lastRejected;
    std::vector<NetEvent> sent;
    std::deque<NetEvent> events;
    std::unordered_set<uint32_t> sessions;
};

struct BatchCounts {
    size_t upserts = 0;
    size_t upsertBytes = 0;
    size_t unloads = 0;
    size_t unloadBatches = 0;
};

BatchCounts checkBatches(const FakeServer& wire, uint32_t id) {
    BatchCounts counts;
    for (const auto& event : wire.sent) {
        if (event.sessionId != id) continue;
        const auto type = getPacketType(event.payload);
        if (type == Payload::ChunkUpsertBatch) {
            std::vector<NetDecodedChunkUpsert> chunks;
            require(deserializeChunkUpsertBatch(event.payload, chunks), "server emitted invalid upsert batch");
            counts.upserts += chunks.size();
            counts.upsertBytes += event.payload.size();
        } else if (type == Payload::ChunkUnloadBatch) {
            std::vector<NetChunkUnload> chunks;
            require(deserializeChunkUnloadBatch(event.payload, chunks), "server emitted invalid unload batch");
            counts.unloads += chunks.size();
            ++counts.unloadBatches;
        }
    }
    require(counts.upserts <= 1024 && counts.upsertBytes <= 128 * 1024, "server exceeded upsert budget");
    return counts;
}

TEST(GameServerNetworkTest, BudgetsCacheAndChunkSendRetry) {
    ensureLogger();
    auto& config = AppConfig::instance();
    config.chunkViewRadiusHorizontal = 14;
    config.chunkViewRadiusVertical = 1;
    config.spawnPosition = {0.0f, 128.0f, 0.0f};
    auto transport = std::make_unique<FakeServer>();
    auto* wire = transport.get();
    GameServer server(std::move(transport));
    wire->rejectedType = Payload::ChunkUpsertBatch;
    wire->connect(1);
    for (int i = 0; i < 20; ++i) {
        server.update(0.0f);
    }
    require(!wire->sent.empty() && getPacketType(wire->sent.front().payload) == Payload::ServerHello, "server hello was not sent directly");
    require(getPacketType(wire->lastRejected) == Payload::ChunkUpsertBatch, "failed upsert path not exercised");
    const auto rejectedUpsert = wire->lastRejected;
    wire->sent.clear();
    wire->rejectedType = Payload::NONE;
    server.update(0.0f);
    const auto first = checkBatches(*wire, 1);
    require(first.upserts == 1024, "full upsert count budget not exercised");
    require(wire->sent.front().payload == rejectedUpsert, "failed core batch was not retained");
    wire->sent.clear();
    server.update(0.0f);
    const auto second = checkBatches(*wire, 1);
    require(second.upserts > 0, "upsert remainder was lost");
    const size_t initialChunks = first.upserts + second.upserts;
    wire->sent.clear();
    server.update(0.0f);
    require(checkBatches(*wire, 1).upserts == 0, "successful batches stayed pending");

    wire->receive(1, serializeClientReady());
    NetClientInput input;
    input.sequence = 1;
    input.position = {2000.0f, 128.0f, 0.0f};
    input.playerMode = PlayerMode::Spectator;
    wire->receive(1, serializeClientInput(input));
    wire->sent.clear();
    server.update(0.0f);
    const auto moved = checkBatches(*wire, 1);
    require(moved.unloads == initialChunks && moved.unloadBatches > 1, "unloads were budgeted or not all sent");
    wire->sent.clear();
    server.update(0.0f);
    require(checkBatches(*wire, 1).unloads == 0, "unloads remained pending after success");

    wire->rejectedType = Payload::ChunkUnloadBatch;
    input.sequence = 2;
    input.position.x += 128.0f;
    wire->receive(1, serializeClientInput(input));
    wire->sent.clear();
    server.update(0.0f);
    require(getPacketType(wire->lastRejected) == Payload::ChunkUnloadBatch, "unload failure path not exercised");
    const auto rejectedUnload = wire->lastRejected;
    wire->rejectedType = Payload::NONE;
    wire->sent.clear();
    server.update(0.0f);
    require(!wire->sent.empty() && wire->sent.front().payload == rejectedUnload, "failed unload batch was lost");

    CommandRequest command;
    command.requestId = 1;
    command.operation = CommandOperation::CreateRobot;
    command.arguments.emplace_back(std::string("QueueTestRobot"));
    wire->receive(1, serializeCommandRequest(command));
    wire->sent.clear();
    server.update(0.0f);
    CommandResponse response;
    require(!wire->sent.empty() && deserializeCommandResponse(wire->sent.front().payload, response) && response.status == CommandStatus::Success, "command response was not sent directly");
    command.requestId = 2;
    command.operation = CommandOperation::DestroyRobot;
    wire->receive(1, serializeCommandRequest(command));
    wire->sent.clear();
    server.update(0.0f);
    require(deserializeCommandResponse(wire->sent.front().payload, response) && response.status == CommandStatus::Success, "created robot missing before destroy command");
    wire->receive(1, serializeClientDisconnect());
    wire->receive(1, serializeCommandRequest(command));
    wire->sent.clear();
    server.update(0.0f);
    require(!wire->hasSession(1), "game did not explicitly close session");
    require(std::none_of(wire->sent.begin(), wire->sent.end(), [](const NetEvent& event) { return event.sessionId == 1; }), "processed command after disconnect");
}

TEST(ChunkCodecBenchmark, ConcentratedEncodeDecodeBurst) {
    const auto data = patternedData(false);
    double encodeTotal = 0.0, encodePeak = 0.0, decodeTotal = 0.0, decodePeak = 0.0;
    size_t rawBytes = 0, encodedBytes = 0;
    constexpr int count = 2048;
    for (int i = 0; i < count; ++i) {
        const auto start = Clock::now();
        const auto encoded = ChunkCodec::encode(data);
        const auto encodedAt = Clock::now();
        ChunkData decoded;
        require(ChunkCodec::decode(encoded.compression, encoded.uncompressedSize, encoded.bytes, decoded), "benchmark decode failed");
        const auto done = Clock::now();
        const double encodeMs = std::chrono::duration<double, std::milli>(encodedAt - start).count();
        const double decodeMs = std::chrono::duration<double, std::milli>(done - encodedAt).count();
        encodeTotal += encodeMs;
        encodePeak = std::max(encodePeak, encodeMs);
        decodeTotal += decodeMs;
        decodePeak = std::max(decodePeak, decodeMs);
        rawBytes += encoded.uncompressedSize;
        encodedBytes += encoded.bytes.size();
    }
    std::cout << "Codec burst (" << count << " patterned chunks): raw=" << rawBytes << " encoded=" << encodedBytes << " encode total/peak ms=" << encodeTotal << '/' << encodePeak << " decode total/peak ms=" << decodeTotal << '/' << decodePeak << '\n';
}

}  // namespace
