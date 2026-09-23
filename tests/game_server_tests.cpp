#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <thread>
#include <vector>

#include "game_server.h"
#include "game_test_support.h"
#include "text.h"

namespace {

using namespace test_support;

class GameServerNetworkTest : public GameTest {};
struct BatchCounts {
    size_t upserts = 0;
    size_t upsertBytes = 0;
    size_t unloads = 0;
    size_t unloadBatches = 0;
};

void checkBatches(const FakeServer& wire, uint32_t id, BatchCounts& counts) {
    counts = {};
    for (const auto& event : wire.sent) {
        if (event.sessionId != id) continue;
        const auto type = getPacketType(event.payload);
        if (type == Payload::ChunkUpsertBatch) {
            std::vector<NetDecodedChunkUpsert> chunks;
            ASSERT_TRUE(deserializeChunkUpsertBatch(event.payload, chunks)) << "server emitted invalid upsert batch";
            counts.upserts += chunks.size();
            counts.upsertBytes += event.payload.size();
        } else if (type == Payload::ChunkUnloadBatch) {
            std::vector<NetChunkUnload> chunks;
            ASSERT_TRUE(deserializeChunkUnloadBatch(event.payload, chunks)) << "server emitted invalid unload batch";
            counts.unloads += chunks.size();
            ++counts.unloadBatches;
        }
    }
    ASSERT_LE(counts.upserts, 1024) << "server exceeded upsert budget";
    ASSERT_LE(counts.upsertBytes, 128 * 1024) << "server exceeded upsert budget";
}

TEST_F(GameServerNetworkTest, BudgetsCacheAndChunkSendRetry) {
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
    ASSERT_FALSE(wire->sent.empty()) << "server hello was not sent directly";
    ASSERT_EQ(getPacketType(wire->sent.front().payload), Payload::ServerHello) << "server hello was not sent directly";
    ASSERT_EQ(getPacketType(wire->lastRejected), Payload::ChunkUpsertBatch) << "failed upsert path not exercised";
    const auto rejectedUpsert = wire->lastRejected;
    wire->sent.clear();
    wire->rejectedType = Payload::NONE;
    server.update(0.0f);
    BatchCounts first;
    ASSERT_NO_FATAL_FAILURE(checkBatches(*wire, 1, first));
    ASSERT_EQ(first.upserts, 1024) << "full upsert count budget not exercised";
    ASSERT_EQ(wire->sent.front().payload, rejectedUpsert) << "failed core batch was not retained";
    wire->sent.clear();
    server.update(0.0f);
    BatchCounts second;
    ASSERT_NO_FATAL_FAILURE(checkBatches(*wire, 1, second));
    ASSERT_GT(second.upserts, 0) << "upsert remainder was lost";
    const size_t initialChunks = first.upserts + second.upserts;
    wire->sent.clear();
    server.update(0.0f);
    BatchCounts settled;
    ASSERT_NO_FATAL_FAILURE(checkBatches(*wire, 1, settled));
    ASSERT_EQ(settled.upserts, 0) << "successful batches stayed pending";

    wire->receive(1, serializeClientReady());
    NetClientInput input;
    input.sequence = 1;
    input.position = {2000.0f, 128.0f, 0.0f};
    input.playerMode = PlayerMode::Spectator;
    wire->receive(1, serializeClientInput(input));
    wire->sent.clear();
    server.update(0.0f);
    BatchCounts moved;
    ASSERT_NO_FATAL_FAILURE(checkBatches(*wire, 1, moved));
    ASSERT_EQ(moved.unloads, initialChunks) << "unloads were budgeted or not all sent";
    ASSERT_GT(moved.unloadBatches, 1) << "unloads were budgeted or not all sent";
    wire->sent.clear();
    server.update(0.0f);
    ASSERT_NO_FATAL_FAILURE(checkBatches(*wire, 1, settled));
    ASSERT_EQ(settled.unloads, 0) << "unloads remained pending after success";

    wire->rejectedType = Payload::ChunkUnloadBatch;
    input.sequence = 2;
    input.position.x += 128.0f;
    wire->receive(1, serializeClientInput(input));
    wire->sent.clear();
    server.update(0.0f);
    ASSERT_EQ(getPacketType(wire->lastRejected), Payload::ChunkUnloadBatch) << "unload failure path not exercised";
    const auto rejectedUnload = wire->lastRejected;
    wire->rejectedType = Payload::NONE;
    wire->sent.clear();
    server.update(0.0f);
    ASSERT_FALSE(wire->sent.empty()) << "failed unload batch was lost";
    ASSERT_EQ(wire->sent.front().payload, rejectedUnload) << "failed unload batch was lost";

    CommandRequest command;
    command.requestId = 1;
    command.operation = CommandOperation::CreateRobot;
    command.arguments.emplace_back(std::string("QueueTestRobot"));
    wire->receive(1, serializeCommandRequest(command));
    wire->sent.clear();
    server.update(0.0f);
    CommandResponse response;
    ASSERT_FALSE(wire->sent.empty()) << "command response was not sent directly";
    ASSERT_TRUE(deserializeCommandResponse(wire->sent.front().payload, response)) << "command response was not sent directly";
    ASSERT_TRUE(response.success) << "command response was not sent directly";
    command.requestId = 2;
    command.operation = CommandOperation::DestroyRobot;
    wire->receive(1, serializeCommandRequest(command));
    wire->sent.clear();
    server.update(0.0f);
    ASSERT_FALSE(wire->sent.empty());
    ASSERT_TRUE(deserializeCommandResponse(wire->sent.front().payload, response)) << "created robot missing before destroy command";
    ASSERT_TRUE(response.success) << "created robot missing before destroy command";
    wire->receive(1, serializeClientDisconnect());
    wire->receive(1, serializeCommandRequest(command));
    wire->sent.clear();
    server.update(0.0f);
    ASSERT_FALSE(wire->hasSession(1)) << "game did not explicitly close session";
    ASSERT_TRUE((std::none_of(wire->sent.begin(), wire->sent.end(), [](const NetEvent& event) { return event.sessionId == 1; }))) << "processed command after disconnect";
}

TEST_F(GameServerNetworkTest, ActorIdsOptionalNamesCommandsAndSessionCleanup) {
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
    NetEntitySnapshot snapshot;
    ASSERT_TRUE(latestActorSnapshot(*wire, 1, snapshot));
    ASSERT_EQ(snapshot.actors.size(), 2u);
    ASSERT_TRUE(latestActorSnapshot(*wire, 2, snapshot));
    ASSERT_EQ(snapshot.actors.size(), 2u);
    wire->receive(1, serializeClientReady());
    wire->receive(2, serializeClientReady());
    for (uint64_t request = 1; request <= 3; ++request) {
        CommandRequest command{request, CommandOperation::CreateRobot, {std::string(request == 3 ? "" : "same")}};
        wire->receive(1, serializeCommandRequest(command));
    }
    wire->sent.clear();
    server.update(0.1f);
    ASSERT_TRUE(latestActorSnapshot(*wire, 1, snapshot));
    ASSERT_EQ(snapshot.actors.size(), 5u);
    std::vector<ActorId> namedRobots;
    for (const auto& actor : snapshot.actors) {
        if (actor.entityType != EntityType::Robot) continue;
        else namedRobots.push_back(actor.id);
    }
    ASSERT_EQ(namedRobots.size(), 3u);
    const auto commandSucceeded = [&](CommandRequest command) -> testing::AssertionResult {
        wire->sent.clear();
        wire->receive(1, serializeCommandRequest(command));
        server.update(0.1f);
        for (const auto& packet : wire->sent) {
            CommandResponse response;
            if (deserializeCommandResponse(packet.payload, response)) {
                if (response.success) return testing::AssertionSuccess();
                return testing::AssertionFailure() << response.message;
            }
        }
        return testing::AssertionFailure() << "No command response";
    };
    ASSERT_TRUE(commandSucceeded({4, CommandOperation::DestroyRobot, {static_cast<int64_t>(namedRobots[0])}}));
    ASSERT_TRUE(latestActorSnapshot(*wire, 1, snapshot));
    EXPECT_EQ(snapshot.actors.size(), 4u);
    ASSERT_TRUE(commandSucceeded({5, CommandOperation::DestroyRobot, {std::string("same")}}));
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
    ASSERT_TRUE(latestActorSnapshot(*wire, 1, snapshot));
    ASSERT_EQ(snapshot.actors.size(), 1u);
    EXPECT_EQ(snapshot.actors[0].id, firstHello.actorId);
    EXPECT_EQ(snapshot.actors[0].position, input.position);
    EXPECT_EQ(snapshot.actors[0].name, "same player");
    wire->receive(1, serializeClientDisconnect());
    server.update(0.0f);
    EXPECT_EQ(probe->world->getEntity(firstHello.actorId), entt::entity(entt::null));
    EXPECT_NE(probe->world->getEntity(secondHello.actorId), entt::entity(entt::null));
    wire->connect(3);
    wire->sent.clear();
    server.update(0.1f);
    NetServerHello reconnect;
    for (const auto& packet : wire->sent) {
        if (packet.sessionId == 3) deserializeServerHello(packet.payload, reconnect);
    }
    EXPECT_GT(reconnect.actorId, firstHello.actorId);
}

TEST_F(GameServerNetworkTest, RobotChunkDemandAndTerrainUnloadPreserveActors) {
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
    ASSERT_NE(probe->world->getEntity(actorId), entt::entity(entt::null));
    probe->world->registry().remove<RobotComponent>(entity);
    server.update(0.0f);
    probe->world->registry().emplace<RobotComponent>(entity);
    server.update(0.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    server.update(0.0f);
    ASSERT_NE(probe->terrain->findChunk({0, 8, 0}), nullptr);
    ASSERT_NE(probe->world->getEntity(actorId), entt::entity(entt::null));
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

TEST_F(GameServerNetworkTest, ConsoleCommandsWorkWithoutAPlayer) {
    GameServer server(std::make_unique<FakeServer>());
    EXPECT_TRUE(server.executeConsoleCommand("help").success);
    for (const auto* text : {"destroy_robot 0", "destroy_robot -1"}) {
        const auto response = server.executeConsoleCommand(text);
        EXPECT_FALSE(response.success);
        EXPECT_EQ(response.message, "Parameter 'id' must be positive.");
    }
    auto response = server.executeConsoleCommand("create_robot");
    EXPECT_FALSE(response.success);
    EXPECT_NE(response.message.find("position"), std::string::npos);
    EXPECT_FALSE(server.executeConsoleCommand("create_robot bad inf 1 2").success);
    EXPECT_FALSE(server.executeConsoleCommand("create_robot bad 65536 1 2").success);
    EXPECT_TRUE(server.executeConsoleCommand("create_robot named 0 128 0").success);
    EXPECT_TRUE(server.executeConsoleCommand("destroy_robot_named named").success);
    EXPECT_FALSE(server.executeConsoleCommand("destroy_robot_named named").success);
    EXPECT_TRUE(server.executeConsoleCommand("create_robot \"\" 0 128 0").success);
    EXPECT_TRUE(server.executeConsoleCommand("destroy_robot").success);
    for (const std::string_view character : {std::string_view("a"), std::string_view("\xe4\xb8\xad"), std::string_view("\xf0\x9f\x98\x80")}) {
        std::string name;
        for (size_t i = 0; i < MAX_NAME_CHARACTERS; ++i) name += character;
        EXPECT_TRUE(server.executeConsoleCommand("create_robot " + name + " 0 128 0").success);
        EXPECT_TRUE(server.executeConsoleCommand("destroy_robot_named " + name).success);
    }
    const std::string longName(MAX_NAME_CHARACTERS + 1, 'a');
    const auto invalidName = server.executeConsoleCommand("create_robot " + longName + " 0 128 0");
    EXPECT_FALSE(invalidName.success);
    EXPECT_NE(invalidName.message.find("characters"), std::string::npos);
    EXPECT_FALSE(server.executeConsoleCommand("destroy_robot_named " + longName).success);
}

TEST_F(GameServerNetworkTest, ChatBroadcastsToEverySessionAndCommandsReplyOnlyToSender) {
    auto transport = std::make_unique<FakeServer>();
    auto* wire = transport.get();
    GameServer server(std::move(transport));
    wire->connect(1);
    wire->connect(2);
    wire->sessions.insert(3);
    wire->events.push_back({NetEventType::Connected, 3, {}});
    server.update(0.0f);
    NetServerHello hello;
    for (const auto& packet : wire->sent) {
        if (packet.sessionId == 1) deserializeServerHello(packet.payload, hello);
    }
    ASSERT_FALSE(hello.actorName.empty());
    wire->receive(1, serializeClientReady());
    wire->sent.clear();
    wire->receive(1, serializeChatRequest("hello everyone"));
    wire->receive(1, serializeCommandRequest({1, CommandOperation::Help, {}}));
    server.update(0.0f);
    std::unordered_set<uint32_t> recipients;
    unsigned responses = 0;
    for (const auto& packet : wire->sent) {
        ChatMessage chat;
        CommandResponse response;
        if (deserializeChatMessage(packet.payload, chat)) {
            recipients.insert(packet.sessionId);
            EXPECT_EQ(chat.name, hello.actorName);
            EXPECT_EQ(chat.text, "hello everyone");
        } else if (deserializeCommandResponse(packet.payload, response)) {
            ++responses;
            EXPECT_EQ(packet.sessionId, 1u);
            EXPECT_TRUE(response.success);
            EXPECT_EQ(response.requestId, 1u);
        }
    }
    EXPECT_EQ(recipients, (std::unordered_set<uint32_t>{1, 2, 3}));
    EXPECT_EQ(responses, 1u);
    wire->sent.clear();
    wire->receive(2, serializeChatRequest("not ready"));
    wire->receive(1, serializeCommandRequest({2, CommandOperation::CreateRobot, {false}}));
    wire->receive(1, serializeCommandRequest({2, CommandOperation::Help, {}}));
    server.update(0.0f);
    unsigned failures = 0;
    for (const auto& packet : wire->sent) {
        EXPECT_NE(getPacketType(packet.payload), Payload::ChatMessage);
        CommandResponse response;
        if (deserializeCommandResponse(packet.payload, response)) {
            ++failures;
            EXPECT_FALSE(response.success);
            EXPECT_FALSE(response.message.empty());
        }
    }
    EXPECT_EQ(failures, 3u);
}

}  // namespace
