#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "game_client.h"
#include "game_server.h"
#include "game_test_support.h"

namespace {

using namespace test_support;

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

class GameClientNetworkTest : public GameTest {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(GameTest::SetUp());
        auto transport = std::make_unique<FakeClient>();
        clientWire_ = transport.get();
        client_ = std::make_unique<GameClient>(nullptr, std::move(transport));
        auto system = std::make_unique<ActorWorldProbe>();
        clientProbe_ = system.get();
        client_->registerSystem(std::move(system));
    }

    void beginSession(const NetServerHello& hello) {
        client_->update(0.0f);
        ASSERT_EQ(client_->state(), GameClient::State::Awaiting) << "client did not send hello";
        clientWire_->receive(serializeServerHello(hello));
        client_->update(0.0f);
        ASSERT_EQ(client_->state(), GameClient::State::Loading) << "client did not enter loading";
    }

    void loadCoreChunks(const NetServerHello& hello) {
        std::vector<NetChunkUpsert> chunks;
        for (const auto pos : hello.coreChunks) {
            chunks.push_back({pos, Chunk(pos).getEncodedSnapshot()});
        }
        clientWire_->receive(serializeChunkUpsertBatch(chunks, builder_));
        client_->update(0.0f);
        ASSERT_TRUE(client_->isSessionReady()) << "client did not finish core terrain loading";
        client_->update(0.0f);
        ASSERT_NE(clientProbe_->world, nullptr) << "client systems did not run";
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
    ASSERT_NO_FATAL_FAILURE(beginSession(hello));
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

TEST_F(GameClientNetworkTest, RenameUnnamedActorsRemovalAndReentry) {
    constexpr entt::entity nullEntity = entt::null;
    NetServerHello hello;
    hello.sessionId = 7;
    hello.actorId = 1;
    hello.actorName = "local";
    hello.position = {1.0f, 2.0f, 3.0f};
    hello.coreChunks = {{0, 0, 0}};
    ASSERT_NO_FATAL_FAILURE(beginSession(hello));
    NetEntitySnapshot snapshot{1, {{1, "renamed local"}, {2, "same"}, {3, "same"}, {4, ""}}};
    for (auto& actor : snapshot.actors) actor.entityType = EntityType::Robot;
    snapshot.actors[0].entityType = EntityType::Player;
    receiveSnapshot(snapshot);
    EXPECT_EQ(client_->state(), GameClient::State::Loading);
    ASSERT_NO_FATAL_FAILURE(loadCoreChunks(hello));
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

TEST_F(GameClientNetworkTest, EntityInterestIndependentOfTerrainAndCoreChunks) {
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
    probe->world->createActor(100, origin + glm::vec3(40.0f, 0.0f, 0.0f));
    probe->world->createActor(101, origin + glm::vec3(-40.0f, 0.0f, 0.0f));
    probe->world->createActor(102, origin + glm::vec3(0.0f, 40.0f, 0.0f));
    const auto coreActor = probe->world->createActor(103, origin + glm::vec3(31.0f, 31.0f, 31.0f));
    probe->world->createActor(104, origin + glm::vec3(31.0f, 0.0f, 0.0f));
    const auto corePos = ChunkLayout::worldToChunk(probe->world->registry().get<TransformComponent>(coreActor).position);
    ASSERT_NE(std::find(hello.coreChunks.begin(), hello.coreChunks.end(), corePos), hello.coreChunks.end());

    NetServerHello clientHello = hello;
    clientHello.coreChunks = {ChunkLayout::worldToChunk(origin)};
    ASSERT_NO_FATAL_FAILURE(beginSession(clientHello));
    ASSERT_NO_FATAL_FAILURE(loadCoreChunks(clientHello));
    auto& clientWorld = *clientProbe_->world;
    const auto local = clientWorld.getEntity(hello.actorId);
    const std::vector<NetChunkUnload> unloads{{clientHello.coreChunks.front(), 1}};
    clientWire_->receive(serializeChunkUnloadBatch(unloads, builder_));
    client_->update(0.0f);
    EXPECT_EQ(clientProbe_->terrain->findChunk(clientHello.coreChunks.front()), nullptr);
    const auto receiveSnapshot = [&](NetEntitySnapshot& snapshot) {
        wire->sent.clear();
        server.update(0.1f);
        EXPECT_TRUE(std::none_of(wire->sent.begin(), wire->sent.end(), [](const NetEvent& event) {
            return getPacketType(event.payload) == Payload::ChunkUpsertBatch;
        }));
        ASSERT_TRUE(latestActorSnapshot(*wire, 1, snapshot));
        this->receiveSnapshot(snapshot);
    };

    NetEntitySnapshot first;
    ASSERT_NO_FATAL_FAILURE(receiveSnapshot(first));
    EXPECT_EQ(first.actors.size(), 5u);
    EXPECT_EQ(getPacketType(wire->lastRejected), Payload::ChunkUpsertBatch);
    EXPECT_NE(clientWorld.getEntity(100), entt::entity(entt::null));
    EXPECT_NE(clientWorld.getEntity(101), entt::entity(entt::null));
    EXPECT_NE(clientWorld.getEntity(102), entt::entity(entt::null));
    EXPECT_EQ(clientWorld.getEntity(103), entt::entity(entt::null));
    EXPECT_NE(clientWorld.getEntity(104), entt::entity(entt::null));

    wire->receive(1, serializeClientReady());
    NetClientInput input;
    input.sequence = 1;
    input.position = origin + glm::vec3(0.5f, 0.0f, 0.0f);
    input.playerMode = PlayerMode::Spectator;
    ASSERT_EQ(ChunkLayout::worldToChunk(input.position), ChunkLayout::worldToChunk(origin));
    wire->receive(1, serializeClientInput(input));
    NetEntitySnapshot moved;
    ASSERT_NO_FATAL_FAILURE(receiveSnapshot(moved));
    EXPECT_EQ(moved.actors.size(), 3u);
    EXPECT_NE(clientWorld.getEntity(100), entt::entity(entt::null));
    EXPECT_EQ(clientWorld.getEntity(101), entt::entity(entt::null));
    EXPECT_EQ(clientWorld.getEntity(102), entt::entity(entt::null));
    EXPECT_NE(probe->world->getEntity(101), entt::entity(entt::null));
    EXPECT_NE(probe->world->getEntity(102), entt::entity(entt::null));

    input.sequence = 2;
    input.position = origin;
    wire->receive(1, serializeClientInput(input));
    NetEntitySnapshot returned;
    ASSERT_NO_FATAL_FAILURE(receiveSnapshot(returned));
    EXPECT_EQ(returned.actors.size(), 5u);
    EXPECT_NE(clientWorld.getEntity(101), entt::entity(entt::null));
    EXPECT_NE(clientWorld.getEntity(102), entt::entity(entt::null));

    config.entityViewRadius = 20.0f;
    ASSERT_NO_FATAL_FAILURE(receiveSnapshot(returned));
    EXPECT_EQ(returned.actors.size(), 1u);
    EXPECT_EQ(clientWorld.getEntity(104), entt::entity(entt::null));
    EXPECT_TRUE(clientWorld.registry().valid(local));
}

TEST_F(GameClientNetworkTest, HeadlessSubmitsChatAndCommandsAndConsumesResults) {
    client_->submitText("/help");
    auto message = client_->consumeMessage();
    ASSERT_TRUE(message);
    EXPECT_FALSE(std::get<CommandResponse>(*message).success);
    NetServerHello hello;
    hello.sessionId = 7;
    hello.actorId = 1;
    hello.actorName = "Player1";
    hello.coreChunks = {{0, 0, 0}};
    ASSERT_NO_FATAL_FAILURE(beginSession(hello));
    clientWire_->receive(serializeChatMessage({"Player2", "while loading"}));
    ASSERT_NO_FATAL_FAILURE(loadCoreChunks(hello));
    message = client_->consumeMessage();
    ASSERT_TRUE(message);
    EXPECT_EQ(std::get<ChatMessage>(*message).text, "while loading");
    clientWire_->sent.clear();
    client_->submitText("/create_robot test");
    client_->submitText("ordinary chat");
    client_->submitCommand("help");
    EXPECT_FALSE(client_->consumeMessage());
    client_->update(0.0f);
    std::vector<CommandRequest> commands;
    unsigned chats = 0;
    for (const auto& packet : clientWire_->sent) {
        CommandRequest command;
        std::string text;
        if (deserializeCommandRequest(packet, command)) commands.push_back(std::move(command));
        else if (deserializeChatRequest(packet, text)) {
            ++chats;
            EXPECT_EQ(text, "ordinary chat");
        }
    }
    ASSERT_EQ(commands.size(), 2u);
    EXPECT_EQ(commands[0].operation, CommandOperation::CreateRobot);
    EXPECT_EQ(commands[1].operation, CommandOperation::Help);
    EXPECT_LT(commands[0].requestId, commands[1].requestId);
    EXPECT_EQ(chats, 1u);
    clientWire_->receive(serializeCommandResponse({commands[0].requestId, false, "Creation failed"}));
    clientWire_->receive(serializeChatMessage({"Player1", "ordinary chat"}));
    client_->update(0.0f);
    message = client_->consumeMessage();
    ASSERT_TRUE(message);
    EXPECT_EQ(std::get<CommandResponse>(*message).message, "Creation failed");
    EXPECT_FALSE(std::get<CommandResponse>(*message).success);
    message = client_->consumeMessage();
    ASSERT_TRUE(message);
    EXPECT_EQ(std::get<ChatMessage>(*message).name, "Player1");
    client_->disconnect();
    message = client_->consumeMessage();
    ASSERT_TRUE(message);
    EXPECT_FALSE(std::get<CommandResponse>(*message).success);
}

}  // namespace
