#include <bgfx/bgfx.h>
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
#include "game_client.h"
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

struct ActorTestConfig {
    AppConfig saved = AppConfig::instance();
    ActorTestConfig() {
        auto& config = AppConfig::instance();
        config.entityViewRadius = 64.0f;
        config.chunkViewRadiusHorizontal = 1;
        config.chunkViewRadiusVertical = 1;
        config.spawnPosition = {0.0f, 128.0f, 0.0f};
    }
    ~ActorTestConfig() { AppConfig::instance() = saved; }
};

class ActorWorldProbe final : public System {
public:
    void update(VoxelWorld& voxels, ActorWorld& actors, float) override {
        world = &actors;
        terrain = &voxels;
    }
    ActorWorld* world = nullptr;
    VoxelWorld* terrain = nullptr;
};

class FakeClient final : public INetClient {
public:
    void connect(const Endpoint&) override { connected_ = true; }
    bool isConnected() const override { return connected_; }
    bool send(std::span<const uint8_t> packet) override {
        if (!connected_) return false;
        sent.emplace_back(packet.begin(), packet.end());
        return true;
    }
    void flush() override {}
    void pump() override {}
    bool popEvent(NetEvent& event) override {
        if (events_.empty()) return false;
        event = std::move(events_.front());
        events_.pop_front();
        return true;
    }
    void close() override { connected_ = false; }
    void receive(std::vector<uint8_t> bytes) {
        events_.push_back(NetEvent{NetEventType::Packet, 0, std::move(bytes)});
    }

    std::vector<std::vector<uint8_t>> sent;

private:
    bool connected_ = true;
    std::deque<NetEvent> events_;
};

class GameClientNetworkTest : public testing::Test {
protected:
    void SetUp() override {
        ensureLogger();
        auto transport = std::make_unique<FakeClient>();
        clientWire_ = transport.get();
        client_ = std::make_unique<GameClient>(nullptr, std::move(transport));
        auto system = std::make_unique<ActorWorldProbe>();
        clientProbe_ = system.get();
        client_->registerSystem(std::move(system));
    }

    void beginSession(const NetServerHello& hello) {
        client_->update(0.0f);
        require(client_->state() == GameClient::State::Awaiting, "client did not send hello");
        clientWire_->receive(serializeServerHello(hello));
        client_->update(0.0f);
        require(client_->state() == GameClient::State::Loading, "client did not enter loading");
    }

    void loadCoreChunks(const NetServerHello& hello) {
        std::vector<NetChunkUpsert> chunks;
        for (const auto pos : hello.coreChunks) {
            chunks.push_back({pos, Chunk(pos).getEncodedSnapshot()});
        }
        clientWire_->receive(serializeChunkUpsertBatch(chunks, builder_));
        client_->update(0.0f);
        require(client_->isSessionReady(), "client did not finish core terrain loading");
        client_->update(0.0f);
        require(clientProbe_->world != nullptr, "client systems did not run");
    }

    void receiveSnapshot(const NetEntitySnapshot& snapshot) {
        std::vector<const NetActorState*> actors;
        actors.reserve(snapshot.actors.size());
        for (const auto& actor : snapshot.actors) {
            actors.push_back(&actor);
        }
        clientWire_->receive(serializeEntitySnapshot(snapshot.sequence, actors, builder_));
        client_->update(0.0f);
    }

    std::unique_ptr<GameClient> client_;
    FakeClient* clientWire_ = nullptr;
    ActorWorldProbe* clientProbe_ = nullptr;
    flatbuffers::FlatBufferBuilder builder_;
};

TEST_F(GameClientNetworkTest, HeadlessWaitsForAllCoreDataThenSendsInputAndDisconnect) {
    NetServerHello hello;
    hello.sessionId = 7;
    hello.actorId = 1;
    hello.position = {1.0f, 2.0f, 3.0f};
    hello.coreChunks = {{0, 0, 0}, {1, 0, 0}};
    beginSession(hello);
    ASSERT_EQ(clientWire_->sent.size(), 1u);
    EXPECT_EQ(getPacketType(clientWire_->sent.front()), Payload::ClientHello);

    Chunk first(hello.coreChunks.front());
    first.applyData(1, ChunkData{BlockType::Stone});
    const std::vector<NetChunkUpsert> partial{{hello.coreChunks.front(), first.getEncodedSnapshot()}};
    clientWire_->receive(serializeChunkUpsertBatch(partial, builder_));
    client_->update(0.0f);
    EXPECT_EQ(client_->state(), GameClient::State::Loading);
    EXPECT_EQ(clientWire_->sent.size(), 1u);

    const std::vector<NetChunkUpsert> remaining{{hello.coreChunks.back(), Chunk(hello.coreChunks.back()).getEncodedSnapshot()}};
    clientWire_->receive(serializeChunkUpsertBatch(remaining, builder_));
    client_->update(0.0f);
    ASSERT_TRUE(client_->isSessionReady());
    EXPECT_EQ(getPacketType(clientWire_->sent.back()), Payload::ClientReady);

    client_->update(0.01f);
    NetClientInput input;
    ASSERT_TRUE(deserializeClientInput(clientWire_->sent.back(), input));
    EXPECT_EQ(input.position, hello.position);
    EXPECT_EQ(input.sequence, 1u);
    ASSERT_NE(clientProbe_->terrain, nullptr);
    EXPECT_EQ(clientProbe_->terrain->getBlock({0, 0, 0}).type, BlockType::Stone);

    client_->disconnect();
    EXPECT_EQ(client_->state(), GameClient::State::Disconnecting);
    EXPECT_EQ(getPacketType(clientWire_->sent.back()), Payload::ClientDisconnect);
}

TEST_F(GameClientNetworkTest, HeadlessConnectionTimeoutClosesTransport) {
    client_->update(0.0f);
    ASSERT_EQ(client_->state(), GameClient::State::Awaiting);
    client_->update(10.0f);
    EXPECT_TRUE(client_->hasFailed());
    EXPECT_FALSE(clientWire_->isConnected());
    EXPECT_EQ(client_->statusText(), "Connection timed out");
}

TEST(ClientChunkManagerTest, HeadlessKeepsTerrainRevisionsWithoutMeshes) {
    VoxelWorld world;
    ClientChunkManager manager(world, false);
    manager.setCoreChunks({{0, 0, 0}, {1, 0, 0}});
    EXPECT_FALSE(manager.areCoreChunksReady());
    EXPECT_TRUE(manager.upsert({0, 0, 0}, 2, ChunkData{BlockType::Stone}));
    EXPECT_FALSE(manager.areCoreChunksReady());
    EXPECT_TRUE(manager.upsert({1, 0, 0}, 1, ChunkData{}));
    EXPECT_TRUE(manager.areCoreChunksReady());
    EXPECT_FALSE(manager.upsert({0, 0, 0}, 1, ChunkData{}));
    EXPECT_EQ(world.getBlock({0, 0, 0}).type, BlockType::Stone);
    EXPECT_FALSE(manager.unload({0, 0, 0}, 1));
    EXPECT_NE(world.findChunk({0, 0, 0}), nullptr);
    EXPECT_TRUE(manager.unload({0, 0, 0}, 2));
    EXPECT_EQ(world.findChunk({0, 0, 0}), nullptr);
    EXPECT_TRUE(manager.upsert({0, 0, 0}, 3, ChunkData{BlockType::Sand}));
    EXPECT_EQ(world.getBlock({0, 0, 0}).type, BlockType::Sand);
    EXPECT_FALSE(manager.takeNextMeshTask());
    EXPECT_EQ(manager.dirtyMeshCount(), 0u);
    EXPECT_EQ(manager.meshCount(), 0u);
    EXPECT_EQ(manager.meshBytesReserved(), 0u);
    EXPECT_TRUE(manager.renderData().chunks.empty());
    EXPECT_EQ(manager.quadIndexBuffer(), UINT16_MAX);
}

TEST(ClientChunkManagerTest, RenderedCoreChunksStillWaitForMeshes) {
    ensureLogger();
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    init.resolution.width = 1;
    init.resolution.height = 1;
    ASSERT_TRUE(bgfx::init(init));
    struct ShutdownRenderer {
        ~ShutdownRenderer() { bgfx::shutdown(); }
    } shutdownRenderer;
    VoxelWorld world;
    ClientChunkManager manager(world);
    manager.setCoreChunks({{0, 0, 0}});
    ASSERT_TRUE(manager.upsert({0, 0, 0}, 1, ChunkData{BlockType::Stone}));
    EXPECT_FALSE(manager.areCoreChunksReady());
    std::optional<ClientChunkManager::MeshTask> task;
    pumpUntil([&] { task = manager.takeNextMeshTask(); }, [&] { return task.has_value(); });
    ChunkMesh mesh;
    buildChunkMesh(world, task->chunkPos, mesh);
    ASSERT_FALSE(mesh.vertices.empty());
    EXPECT_EQ(manager.completeMeshTask(*task, mesh), ClientChunkManager::MeshTaskResult::Accepted);
    EXPECT_TRUE(manager.areCoreChunksReady());
}

NetEntitySnapshot latestActorSnapshot(const FakeServer& wire, uint32_t sessionId) {
    for (auto it = wire.sent.rbegin(); it != wire.sent.rend(); ++it) {
        NetEntitySnapshot snapshot;
        if (it->sessionId == sessionId && deserializeEntitySnapshot(it->payload, snapshot)) {
            return snapshot;
        }
    }
    throw std::runtime_error("No entity snapshot received");
}

TEST_F(GameClientNetworkTest, RenameUnnamedActorsRemovalAndReentry) {
    constexpr entt::entity nullEntity = entt::null;
    NetServerHello hello;
    hello.sessionId = 7;
    hello.actorId = 1;
    hello.actorName = "local";
    hello.position = {1.0f, 2.0f, 3.0f};
    hello.coreChunks = {{0, 0, 0}};
    beginSession(hello);
    NetEntitySnapshot snapshot{1, {{1, "renamed local"}, {2, "same"}, {3, "same"}, {4, ""}}};
    for (auto& actor : snapshot.actors) actor.entityType = EntityType::Robot;
    snapshot.actors[0].entityType = EntityType::Player;
    receiveSnapshot(snapshot);
    EXPECT_EQ(client_->state(), GameClient::State::Loading);
    loadCoreChunks(hello);
    auto& world = *clientProbe_->world;
    const auto local = world.getEntity(1);
    const auto localEffect = world.createActor(99, {});
    const auto remote = world.getEntity(2);
    ASSERT_NE(remote, nullEntity);
    EXPECT_EQ(world.registry().get<TransformComponent>(local).position, glm::vec3(1.0f, 2.0f, 3.0f));
    EXPECT_EQ(world.registry().get<NameComponent>(local).name, "renamed local");
    EXPECT_TRUE(world.registry().all_of<RandomMovementComponent>(remote));
    EXPECT_EQ(world.registry().get<ReplicationStateComponent>(remote).lastSnapshotSequence, 1u);
    EXPECT_FALSE(world.registry().all_of<ReplicationStateComponent>(local));
    EXPECT_FALSE(world.registry().all_of<NameComponent>(world.getEntity(4)));
    snapshot.sequence = 2;
    snapshot.actors = {{2, "new name", {16.0f, 0.0f, 0.0f}}, {4, ""}};
    receiveSnapshot(snapshot);
    EXPECT_EQ(world.getEntity(2), remote);
    EXPECT_EQ(world.registry().get<NameComponent>(remote).name, "new name");
    EXPECT_EQ(world.registry().get<InterpolationComponent>(remote).samples.size(), 2u);
    EXPECT_EQ(world.getEntity(3), nullEntity);
    EXPECT_TRUE(world.registry().valid(local));
    EXPECT_TRUE(world.registry().valid(localEffect));
    receiveSnapshot(NetEntitySnapshot{1, {}});
    EXPECT_EQ(world.getEntity(2), remote);
    receiveSnapshot(NetEntitySnapshot{3, {{2, ""}}});
    EXPECT_FALSE(world.registry().all_of<NameComponent>(remote));
    receiveSnapshot(NetEntitySnapshot{4, {}});
    EXPECT_EQ(world.getEntity(2), nullEntity);
    receiveSnapshot(NetEntitySnapshot{5, {{2, "back"}}});
    EXPECT_NE(world.getEntity(2), nullEntity);
    EXPECT_EQ(world.registry().get<InterpolationComponent>(world.getEntity(2)).samples.size(), 1u);
    client_->disconnect();
    receiveSnapshot(NetEntitySnapshot{6, {}});
    EXPECT_EQ(client_->state(), GameClient::State::Disconnecting);
    EXPECT_NE(world.getEntity(2), nullEntity);
}

TEST(GameServerNetworkTest, ActorIdsOptionalNamesCommandsAndSessionCleanup) {
    ensureLogger();
    ActorTestConfig config;
    auto transport = std::make_unique<FakeServer>();
    auto* wire = transport.get();
    GameServer server(std::move(transport));
    auto system = std::make_unique<ActorWorldProbe>();
    auto* probe = system.get();
    server.registerSystem(std::move(system));
    wire->connect(1);
    wire->connect(2);
    server.update(0.1f);
    NetServerHello firstHello, secondHello;
    for (const auto& packet : wire->sent) {
        if (packet.sessionId == 1) deserializeServerHello(packet.payload, firstHello);
        if (packet.sessionId == 2) deserializeServerHello(packet.payload, secondHello);
    }
    ASSERT_NE(firstHello.actorId, 0u);
    ASSERT_NE(firstHello.actorId, secondHello.actorId);
    ASSERT_EQ(latestActorSnapshot(*wire, 1).actors.size(), 2u);
    ASSERT_EQ(latestActorSnapshot(*wire, 2).actors.size(), 2u);
    wire->receive(1, serializeClientReady());
    wire->receive(2, serializeClientReady());
    for (uint64_t request = 1; request <= 3; ++request) {
        CommandRequest command{request, CommandOperation::CreateRobot, {std::string(request == 3 ? "" : "same")}};
        wire->receive(1, serializeCommandRequest(command));
    }
    wire->sent.clear();
    server.update(0.1f);
    auto snapshot = latestActorSnapshot(*wire, 1);
    ASSERT_EQ(snapshot.actors.size(), 5u);
    std::vector<ActorId> namedRobots;
    for (const auto& actor : snapshot.actors) {
        if (actor.entityType != EntityType::Robot) continue;
        else namedRobots.push_back(actor.id);
    }
    const auto commandStatus = [&](CommandRequest command) {
        wire->sent.clear();
        wire->receive(1, serializeCommandRequest(command));
        server.update(0.1f);
        for (const auto& packet : wire->sent) {
            CommandResponse response;
            if (deserializeCommandResponse(packet.payload, response)) return response.status;
        }
        throw std::runtime_error("No command response");
    };
    EXPECT_EQ(commandStatus({4, CommandOperation::DestroyRobot, {static_cast<int64_t>(namedRobots[0])}}), CommandStatus::Success);
    EXPECT_EQ(latestActorSnapshot(*wire, 1).actors.size(), 4u);
    EXPECT_EQ(commandStatus({5, CommandOperation::DestroyRobot, {std::string("same")}}), CommandStatus::Success);
    ASSERT_NE(probe->world, nullptr);
    probe->world->setName(probe->world->getEntity(firstHello.actorId), "same player");
    probe->world->setName(probe->world->getEntity(secondHello.actorId), "same player");
    NetClientInput input;
    input.sequence = 1;
    input.position = {160.0f, 128.0f, 0.0f};
    input.playerMode = PlayerMode::Spectator;
    wire->receive(1, serializeClientInput(input));
    wire->sent.clear();
    server.update(0.1f);
    snapshot = latestActorSnapshot(*wire, 1);
    ASSERT_EQ(snapshot.actors.size(), 1u);
    EXPECT_EQ(snapshot.actors[0].id, firstHello.actorId);
    EXPECT_EQ(snapshot.actors[0].position, input.position);
    EXPECT_EQ(snapshot.actors[0].name, "same player");
    wire->receive(1, serializeClientDisconnect());
    server.update(0.0f);
    EXPECT_TRUE(probe->world->getEntity(firstHello.actorId) == entt::null);
    EXPECT_TRUE(probe->world->getEntity(secondHello.actorId) != entt::null);
    wire->connect(3);
    wire->sent.clear();
    server.update(0.1f);
    NetServerHello reconnect;
    for (const auto& packet : wire->sent) {
        if (packet.sessionId == 3) deserializeServerHello(packet.payload, reconnect);
    }
    EXPECT_GT(reconnect.actorId, firstHello.actorId);
}

TEST_F(GameClientNetworkTest, EntityInterestIndependentOfTerrainAndCoreChunks) {
    ensureLogger();
    ActorTestConfig savedConfig;
    auto& config = AppConfig::instance();
    config.entityViewRadius = 40.0f;
    auto transport = std::make_unique<FakeServer>();
    auto* wire = transport.get();
    wire->rejectedType = Payload::ChunkUpsertBatch;
    GameServer server(std::move(transport));
    auto system = std::make_unique<ActorWorldProbe>();
    auto* probe = system.get();
    server.registerSystem(std::move(system));
    wire->connect(1);
    server.update(0.1f);
    NetServerHello hello;
    for (const auto& packet : wire->sent) deserializeServerHello(packet.payload, hello);
    ASSERT_NE(hello.actorId, 0u);
    ASSERT_NE(probe->world, nullptr);
    const glm::vec3 origin = config.spawnPosition;
    // No physics components: these actors stay exactly on their test boundaries.
    probe->world->createActor(100, origin + glm::vec3(40.0f, 0.0f, 0.0f));
    probe->world->createActor(101, origin + glm::vec3(-40.0f, 0.0f, 0.0f));
    probe->world->createActor(102, origin + glm::vec3(0.0f, 40.0f, 0.0f));
    const auto coreActor = probe->world->createActor(103, origin + glm::vec3(31.0f, 31.0f, 31.0f));
    probe->world->createActor(104, origin + glm::vec3(31.0f, 0.0f, 0.0f));
    const auto corePos = ChunkLayout::worldToChunk(probe->world->registry().get<TransformComponent>(coreActor).position);
    ASSERT_NE(std::find(hello.coreChunks.begin(), hello.coreChunks.end(), corePos), hello.coreChunks.end());

    NetServerHello clientHello = hello;
    clientHello.coreChunks = {ChunkLayout::worldToChunk(origin)};
    beginSession(clientHello);
    loadCoreChunks(clientHello);
    auto& clientWorld = *clientProbe_->world;
    const auto local = clientWorld.getEntity(hello.actorId);
    // Finish startup, then unload all client terrain before applying snapshots.
    const std::vector<NetChunkUnload> unloads{{clientHello.coreChunks.front(), 1}};
    clientWire_->receive(serializeChunkUnloadBatch(unloads, builder_));
    client_->update(0.0f);
    EXPECT_EQ(clientProbe_->terrain->findChunk(clientHello.coreChunks.front()), nullptr);
    const auto receiveSnapshot = [&] {
        wire->sent.clear();
        server.update(0.1f);
        EXPECT_TRUE(std::none_of(wire->sent.begin(), wire->sent.end(), [](const NetEvent& event) {
            return getPacketType(event.payload) == Payload::ChunkUpsertBatch;
        }));
        const auto snapshot = latestActorSnapshot(*wire, 1);
        this->receiveSnapshot(snapshot);
        return snapshot;
    };

    const auto first = receiveSnapshot();
    EXPECT_EQ(first.actors.size(), 5u);
    EXPECT_EQ(getPacketType(wire->lastRejected), Payload::ChunkUpsertBatch);
    EXPECT_TRUE(clientWorld.getEntity(100) != entt::null);  // Outside the client's chunk interest.
    EXPECT_TRUE(clientWorld.getEntity(101) != entt::null);
    EXPECT_TRUE(clientWorld.getEntity(102) != entt::null);
    EXPECT_TRUE(clientWorld.getEntity(103) == entt::null);  // Core terrain does not force entity visibility.
    EXPECT_TRUE(clientWorld.getEntity(104) != entt::null);

    wire->receive(1, serializeClientReady());
    NetClientInput input;
    input.sequence = 1;
    input.position = origin + glm::vec3(0.5f, 0.0f, 0.0f);
    input.playerMode = PlayerMode::Spectator;
    ASSERT_EQ(ChunkLayout::worldToChunk(input.position), ChunkLayout::worldToChunk(origin));
    wire->receive(1, serializeClientInput(input));
    const auto moved = receiveSnapshot();
    EXPECT_EQ(moved.actors.size(), 3u);
    EXPECT_TRUE(clientWorld.getEntity(100) != entt::null);
    EXPECT_TRUE(clientWorld.getEntity(101) == entt::null);
    EXPECT_TRUE(clientWorld.getEntity(102) == entt::null);  // Y contributes to the spherical distance.
    EXPECT_TRUE(probe->world->getEntity(101) != entt::null);
    EXPECT_TRUE(probe->world->getEntity(102) != entt::null);

    input.sequence = 2;
    input.position = origin;
    wire->receive(1, serializeClientInput(input));
    EXPECT_EQ(receiveSnapshot().actors.size(), 5u);
    EXPECT_TRUE(clientWorld.getEntity(101) != entt::null);
    EXPECT_TRUE(clientWorld.getEntity(102) != entt::null);

    config.entityViewRadius = 20.0f;
    EXPECT_EQ(receiveSnapshot().actors.size(), 1u);
    EXPECT_TRUE(clientWorld.getEntity(104) == entt::null);  // Still inside terrain view, outside entity radius.
    EXPECT_TRUE(clientWorld.registry().valid(local));
}

TEST(GameServerNetworkTest, RobotChunkDemandAndTerrainUnloadPreserveActors) {
    ensureLogger();
    ActorTestConfig config;
    auto transport = std::make_unique<FakeServer>();
    auto* wire = transport.get();
    GameServer server(std::move(transport));
    auto system = std::make_unique<ActorWorldProbe>();
    auto* probe = system.get();
    server.registerSystem(std::move(system));
    wire->connect(1);
    for (int i = 0; i < 10; ++i) server.update(0.0f);
    ASSERT_NE(probe->world, nullptr);
    constexpr ActorId actorId = 10000;
    const auto entity = probe->world->createRobot(actorId, "retained", {0.0f, 128.0f, 0.0f});
    probe->world->registry().get<TransformComponent>(entity).scale = glm::vec3(2.0f);
    wire->receive(1, serializeClientReady());
    NetClientInput input;
    input.sequence = 1;
    input.position = {512.0f, 128.0f, 0.0f};
    input.playerMode = PlayerMode::Spectator;
    wire->receive(1, serializeClientInput(input));
    server.update(0.0f);
    ASSERT_TRUE(probe->world->getEntity(actorId) != entt::null);
    // Removing RobotComponent releases demand; adding it back must restore demand.
    probe->world->registry().remove<RobotComponent>(entity);
    server.update(0.0f);
    probe->world->registry().emplace<RobotComponent>(entity);
    server.update(0.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    server.update(0.0f);
    ASSERT_NE(probe->terrain->findChunk({0, 8, 0}), nullptr);
    ASSERT_TRUE(probe->world->getEntity(actorId) != entt::null);
    probe->world->registry().remove<RobotComponent>(entity);
    server.update(0.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    server.update(0.0f);
    EXPECT_EQ(probe->terrain->findChunk({0, 8, 0}), nullptr);
    EXPECT_EQ(probe->world->getEntity(actorId), entity);
    input.sequence = 2;
    input.position = {0.0f, 128.0f, 0.0f};
    wire->receive(1, serializeClientInput(input));
    for (int i = 0; i < 10; ++i) server.update(0.0f);
    const auto retained = probe->world->getEntity(actorId);
    ASSERT_EQ(retained, entity);
    EXPECT_NE(probe->terrain->findChunk({0, 8, 0}), nullptr);
    EXPECT_FALSE(probe->world->registry().all_of<RobotComponent>(retained));
    EXPECT_EQ(probe->world->registry().get<NameComponent>(retained).name, "retained");
    EXPECT_EQ(probe->world->registry().get<TransformComponent>(retained).scale, glm::vec3(2.0f));
    EXPECT_EQ(probe->world->registry().get<TransformComponent>(retained).position, glm::vec3(0.0f, 128.0f, 0.0f));
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
