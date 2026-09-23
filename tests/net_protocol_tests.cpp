#include <gtest/gtest.h>

#include <limits>

#include "chunk.h"
#include "chunk_test_support.h"
#include "command.h"
#include "net_protocol.h"
#include "test_support.h"
#include "text.h"

namespace {

using namespace test_support;

using Payload = mineworld::net::NetMessagePayload;
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
    ASSERT_FALSE(bytes.empty()) << "64-entry batch failed";
    ASSERT_LE(bytes.size(), MAX_CHUNK_BATCH_BYTES) << "64-entry batch failed";
    std::vector<NetDecodedChunkUpsert> decoded;
    ASSERT_TRUE(deserializeChunkUpsertBatch(bytes, decoded)) << "batch round trip failed";
    ASSERT_EQ(decoded.size(), 64) << "batch round trip failed";
    ASSERT_NO_FATAL_FAILURE(sameBlocks(decoded.back().blocks, chunk.getData()));
    ASSERT_EQ(decoded.back().chunkPos.x, 63) << "batch metadata changed";
    ASSERT_EQ(decoded.back().revision, 9) << "batch metadata changed";
    ASSERT_FALSE(deserializeChunkUpsertBatch(malformedBatch(255, 3), decoded)) << "partially published unknown-codec batch";
    ASSERT_EQ(decoded.size(), 64) << "partially published unknown-codec batch";
    ASSERT_FALSE(deserializeChunkUpsertBatch(malformedBatch(0, 4), decoded)) << "partially published invalid-length batch";
    ASSERT_EQ(decoded.size(), 64) << "partially published invalid-length batch";
    ASSERT_FALSE(deserializeChunkUpsertBatch(std::span(bytes).first(bytes.size() / 2), decoded)) << "accepted truncated FlatBuffer";
    entries.push_back(entries.back());
    ASSERT_TRUE(serializeChunkUpsertBatch(entries, builder).empty()) << "accepted 65-entry batch";
    ASSERT_TRUE((serializeChunkUpsertBatch({}, builder).empty())) << "accepted empty batch";

    auto raw = std::make_shared<EncodedChunkSnapshot>();
    raw->revision = 1;
    const auto noise = patternedData(true);
    noise.serialize(raw->data.bytes);
    raw->data.uncompressedSize = static_cast<uint32_t>(raw->data.bytes.size());
    entries.assign(64, NetChunkUpsert{{0, 0, 0}, raw});
    ASSERT_TRUE(serializeChunkUpsertBatch(entries, builder).empty()) << "accepted batch over 32 KiB";
    while ((bytes = serializeChunkUpsertBatch(entries, builder)).empty()) {
        ASSERT_FALSE(entries.empty()) << "No batch fits within the byte limit";
        entries.pop_back();
    }
    ASSERT_LE(bytes.size(), MAX_CHUNK_BATCH_BYTES) << "byte-boundary batch failed";
    ASSERT_TRUE(deserializeChunkUpsertBatch(bytes, decoded)) << "byte-boundary batch failed";
    entries.push_back(entries.back());
    ASSERT_TRUE(serializeChunkUpsertBatch(entries, builder).empty()) << "boundary plus one was accepted";

    std::vector<NetChunkUnload> unloads(256, NetChunkUnload{{0, 0, 0}, 9});
    std::vector<NetChunkUnload> unloadResult;
    bytes = serializeChunkUnloadBatch(unloads, builder);
    ASSERT_TRUE(deserializeChunkUnloadBatch(bytes, unloadResult)) << "unload round trip failed";
    ASSERT_EQ(unloadResult.size(), 256) << "unload round trip failed";
    ASSERT_EQ(unloadResult[0].revision, 9) << "unload revision changed";
    unloads.push_back(unloads.back());
    ASSERT_TRUE(serializeChunkUnloadBatch(unloads, builder).empty()) << "accepted 257 unloads";
    ASSERT_FALSE(deserializeChunkUpsertBatch(bytes, decoded)) << "accepted unload as upsert";
    unloads.resize(1);
    unloads[0].chunkPos.y = 99;
    ASSERT_TRUE(serializeChunkUnloadBatch(unloads, builder).empty()) << "accepted out-of-world chunk";
}

TEST(ActorProtocolTest, IdNamesAndHelloRoundTrip) {
    flatbuffers::FlatBufferBuilder builder;
    NetEntitySnapshot snapshot{1, {{1, ""}, {2, "同名"}, {3, "同名"}}};
    snapshot.actors[0].entityType = EntityType::Robot;
    snapshot.actors[0].velocity = {1.0f, 2.0f, 3.0f};
    std::vector<const NetActorState*> actors;
    for (const auto& actor : snapshot.actors) {
        actors.push_back(&actor);
    }
    const auto bytes = serializeEntitySnapshot(snapshot.sequence, actors, builder);
    ASSERT_FALSE(bytes.empty());
    const auto* wire = mineworld::net::GetNetMessage(bytes.data())->payload_as_EntitySnapshot();
    ASSERT_NE(wire, nullptr);
    EXPECT_EQ(wire->actors()->Get(0)->name(), nullptr);
    NetEntitySnapshot decoded;
    ASSERT_TRUE(deserializeEntitySnapshot(bytes, decoded));
    ASSERT_EQ(decoded.actors.size(), 3u);
    EXPECT_EQ(decoded.actors[0].id, 1u);
    EXPECT_TRUE(decoded.actors[0].name.empty());
    EXPECT_EQ(decoded.actors[1].name, "同名");
    EXPECT_EQ(decoded.actors[2].name, "同名");
    EXPECT_EQ(decoded.actors[0].velocity, glm::vec3(1.0f, 2.0f, 3.0f));
    snapshot.actors[2].id = 2;
    EXPECT_TRUE(deserializeEntitySnapshot(serializeEntitySnapshot(snapshot.sequence, actors, builder), decoded));
    EXPECT_EQ(decoded.actors[2].id, 2u);
    snapshot.actors[0].id = 0;
    EXPECT_TRUE(serializeEntitySnapshot(snapshot.sequence, actors, builder).empty());
    NetServerHello hello;
    hello.sessionId = 7;
    hello.actorId = std::numeric_limits<ActorId>::max();
    NetServerHello restored;
    ASSERT_TRUE(deserializeServerHello(serializeServerHello(hello), restored));
    EXPECT_EQ(restored.actorId, hello.actorId);
    EXPECT_TRUE(restored.actorName.empty());
    hello.actorId = 0;
    EXPECT_FALSE(deserializeServerHello(serializeServerHello(hello), restored));
}

TEST(NameProtocolTest, RoundTripsMaximumUnicodeNames) {
    for (std::string_view character : {"a", "\xe4\xb8\xad", "\xf0\x9f\x98\x80"}) {
        const auto name = repeated(character, MAX_NAME_CHARACTERS);
        NetServerHello hello;
        hello.actorId = 1;
        hello.actorName = name;
        NetServerHello decodedHello;
        ASSERT_TRUE(deserializeServerHello(serializeServerHello(hello), decodedHello));
        EXPECT_EQ(decodedHello.actorName, name);

        flatbuffers::FlatBufferBuilder builder;
        const NetActorState actor{1, name};
        const NetActorState* actors[] = {&actor};
        NetEntitySnapshot decodedSnapshot;
        ASSERT_TRUE(deserializeEntitySnapshot(serializeEntitySnapshot(1, actors, builder), decodedSnapshot));
        ASSERT_EQ(decodedSnapshot.actors.size(), 1u);
        EXPECT_EQ(decodedSnapshot.actors.front().name, name);

        ChatMessage decodedChat;
        ASSERT_TRUE(deserializeChatMessage(serializeChatMessage({name, "hello"}), decodedChat));
        EXPECT_EQ(decodedChat.name, name);
    }
}

TEST(NameProtocolTest, RejectsInvalidNamesOnBothSides) {
    using Payload = mineworld::net::NetMessagePayload;
    const std::string invalidNames[] = {std::string(MAX_NAME_CHARACTERS + 1, 'a'), repeated("\xe4\xb8\xad", MAX_NAME_CHARACTERS + 1), "bad\nname", "\xc0\xaf"};
    for (const auto& name : invalidNames) {
        NetServerHello hello;
        hello.actorId = 1;
        hello.actorName = name;
        EXPECT_TRUE(serializeServerHello(hello).empty());
        flatbuffers::FlatBufferBuilder builder;
        const NetActorState actor{1, name};
        const NetActorState* actors[] = {&actor};
        EXPECT_TRUE(serializeEntitySnapshot(1, actors, builder).empty());
        EXPECT_TRUE(serializeChatMessage({name, "hello"}).empty());

        builder.Clear();
        const auto wireHello = mineworld::net::CreateServerHello(builder, 7, 1, builder.CreateString(name));
        mineworld::net::FinishNetMessageBuffer(builder, mineworld::net::CreateNetMessage(builder, Payload::ServerHello, wireHello.Union()));
        NetServerHello decodedHello;
        decodedHello.actorName = "unchanged";
        EXPECT_FALSE(deserializeServerHello({builder.GetBufferPointer(), builder.GetSize()}, decodedHello));
        EXPECT_EQ(decodedHello.actorName, "unchanged");

        builder.Clear();
        const auto wireActor = mineworld::net::CreateActorState(builder, 1, builder.CreateString(name));
        const auto wireSnapshot = mineworld::net::CreateEntitySnapshot(builder, 1, builder.CreateVector(&wireActor, 1));
        mineworld::net::FinishNetMessageBuffer(builder, mineworld::net::CreateNetMessage(builder, Payload::EntitySnapshot, wireSnapshot.Union()));
        NetEntitySnapshot decodedSnapshot{42, {}};
        EXPECT_FALSE(deserializeEntitySnapshot({builder.GetBufferPointer(), builder.GetSize()}, decodedSnapshot));
        EXPECT_EQ(decodedSnapshot.sequence, 42u);

        builder.Clear();
        const auto wireChat = mineworld::net::CreateChatMessage(builder, builder.CreateString(name), builder.CreateString("hello"));
        mineworld::net::FinishNetMessageBuffer(builder, mineworld::net::CreateNetMessage(builder, Payload::ChatMessage, wireChat.Union()));
        ChatMessage decodedChat{"unchanged", "old"};
        EXPECT_FALSE(deserializeChatMessage({builder.GetBufferPointer(), builder.GetSize()}, decodedChat));
        EXPECT_EQ(decodedChat.name, "unchanged");
    }
}

TEST(ChatProtocolTest, RoundTripLimitsAndUnicodeValidation) {
    const std::string text = "Hello \xe4\xb8\x96\xe7\x95\x8c";
    std::string decoded;
    ASSERT_TRUE(deserializeChatRequest(serializeChatRequest(text), decoded));
    EXPECT_EQ(decoded, text);
    ChatMessage chat;
    ASSERT_TRUE(deserializeChatMessage(serializeChatMessage({"Player7", text}), chat));
    EXPECT_EQ(chat.name, "Player7");
    EXPECT_EQ(chat.text, text);
    EXPECT_TRUE(serializeChatRequest(std::string(1025, 'a')).empty());
    EXPECT_TRUE(serializeChatRequest(" \t ").empty());
    EXPECT_TRUE(serializeChatRequest("bad\nline").empty());
    EXPECT_TRUE(serializeChatRequest("bad\x1b").empty());
    EXPECT_TRUE(serializeChatRequest("\xc0\xaf").empty());
    EXPECT_TRUE(serializeChatRequest("\xed\xa0\x80").empty());
    EXPECT_TRUE(serializeChatRequest("\xf4\x90\x80\x80").empty());
    EXPECT_TRUE(serializeChatMessage({std::string(65, 'a'), text}).empty());
    const auto bytes = serializeChatMessage({"Player7", text});
    EXPECT_FALSE(deserializeChatMessage(std::span(bytes).first(bytes.size() / 2), chat));
    EXPECT_FALSE(deserializeChatRequest(bytes, decoded));
    flatbuffers::FlatBufferBuilder builder;
    const auto request = mineworld::net::CreateChatRequest(builder, builder.CreateString(std::string(1025, 'a')));
    const auto message = mineworld::net::CreateNetMessage(builder, mineworld::net::NetMessagePayload::ChatRequest, request.Union());
    mineworld::net::FinishNetMessageBuffer(builder, message);
    EXPECT_FALSE(deserializeChatRequest({builder.GetBufferPointer(), builder.GetSize()}, decoded));
    EXPECT_EQ(decoded, text);
}

TEST(ChatProtocolTest, RejectsMissingTextAndName) {
    using namespace mineworld::net;
    flatbuffers::FlatBufferBuilder builder;
    const auto request = CreateChatRequest(builder);
    FinishNetMessageBuffer(builder, CreateNetMessage(builder, NetMessagePayload::ChatRequest, request.Union()));
    const std::span requestBytes(builder.GetBufferPointer(), builder.GetSize());
    EXPECT_EQ(getPacketType(requestBytes), NetMessagePayload::ChatRequest);
    std::string text = "unchanged";
    EXPECT_FALSE(deserializeChatRequest(requestBytes, text));
    EXPECT_EQ(text, "unchanged");

    for (bool includeName : {false, true}) {
        builder.Clear();
        const auto presentField = builder.CreateString("Present");
        const auto missingField = flatbuffers::Offset<flatbuffers::String>{};
        const auto chat = CreateChatMessage(builder, includeName ? presentField : missingField, includeName ? missingField : presentField);
        FinishNetMessageBuffer(builder, CreateNetMessage(builder, NetMessagePayload::ChatMessage, chat.Union()));
        const std::span bytes(builder.GetBufferPointer(), builder.GetSize());
        EXPECT_EQ(getPacketType(bytes), NetMessagePayload::ChatMessage);
        ::ChatMessage decoded{"old name", "old text"};
        EXPECT_FALSE(deserializeChatMessage(bytes, decoded));
        EXPECT_EQ(decoded.name, "old name");
        EXPECT_EQ(decoded.text, "old text");
    }
}

TEST(CommandProtocolTest, RejectsMissingResponseMessage) {
    using namespace mineworld::net;
    flatbuffers::FlatBufferBuilder builder;
    const auto response = CreateCommandResponse(builder, 42, true);
    FinishNetMessageBuffer(builder, CreateNetMessage(builder, NetMessagePayload::CommandResponse, response.Union()));
    const std::span bytes(builder.GetBufferPointer(), builder.GetSize());
    EXPECT_EQ(getPacketType(bytes), NetMessagePayload::CommandResponse);
    ::CommandResponse decoded{7, false, "unchanged"};
    EXPECT_FALSE(deserializeCommandResponse(bytes, decoded));
    EXPECT_EQ(decoded.requestId, 7u);
    EXPECT_FALSE(decoded.success);
    EXPECT_EQ(decoded.message, "unchanged");
}

TEST(CommandProtocolTest, ResponseSuccessMessageAndTypedRequestRoundTrip) {
    for (bool success : {true, false}) {
        CommandResponse response;
        ASSERT_TRUE(deserializeCommandResponse(serializeCommandResponse({42, success, "Result\nMore detail"}), response));
        EXPECT_EQ(response.requestId, 42u);
        EXPECT_EQ(response.success, success);
        EXPECT_EQ(response.message, "Result\nMore detail");
    }
    auto parsed = parseCommandLine("create_robot name 1 2 3");
    ASSERT_TRUE(parsed.command);
    CommandRequest request;
    ASSERT_TRUE(deserializeCommandRequest(serializeCommandRequest(*parsed.command), request));
    EXPECT_TRUE(validateCommand(request).empty());
    EXPECT_EQ(std::get<glm::vec3>(request.arguments[1]), glm::vec3(1, 2, 3));
}

}  // namespace
