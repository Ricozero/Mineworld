#include <gtest/gtest.h>

#include <limits>

#include "actor_world.h"
#include "net_protocol.h"
#include "server_actor_manager.h"

namespace {

constexpr entt::entity nullEntity = entt::null;

TEST(ActorWorldTest, IdentityOptionalNamesAndRegistryDestruction) {
    ActorWorld world;
    ServerActorManager manager(world);
    EXPECT_EQ(world.createActor(0, {}), nullEntity);
    const auto firstId = manager.allocateId();
    const auto first = world.createRobot(firstId, "", {});
    const auto second = world.createRobot(manager.allocateId(), "same", {});
    const auto third = world.createRobot(manager.allocateId(), "same", {});
    ASSERT_NE(first, nullEntity);
    EXPECT_NE(second, third);
    EXPECT_FALSE(world.registry().all_of<NameComponent>(first));
    EXPECT_TRUE(world.registry().all_of<RandomMovementComponent>(first));
    EXPECT_EQ(world.createRobot(firstId, "duplicate ID", {}), nullEntity);
    world.setName(first, "same");
    EXPECT_EQ(world.findActorsByName("same").size(), 3u);
    world.setName(first, "");
    EXPECT_FALSE(world.registry().all_of<NameComponent>(first));
    world.registry().destroy(first);
    EXPECT_EQ(world.getEntity(firstId), nullEntity);
    EXPECT_FALSE(world.destroyActor(firstId));
    const auto secondId = world.registry().get<ActorComponent>(second).id;
    EXPECT_TRUE(world.destroyActor(secondId));
    EXPECT_EQ(world.getEntity(secondId), nullEntity);
    EXPECT_GT(manager.allocateId(), secondId);
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

}  // namespace
