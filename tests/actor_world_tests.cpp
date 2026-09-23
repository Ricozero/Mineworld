#include <gtest/gtest.h>

#include "actor_world.h"
#include "server_actor_manager.h"
#include "test_support.h"
#include "text.h"

namespace {

using namespace test_support;

constexpr entt::entity kNullEntity = entt::null;

TEST(ActorWorldTest, IdentityOptionalNamesAndDestruction) {
    ActorWorld world;
    ServerActorManager manager(world);
    EXPECT_EQ(world.createActor(0, {}), kNullEntity);
    const auto firstId = manager.allocateId();
    const auto first = world.createRobot(firstId, "", {});
    const auto second = world.createRobot(manager.allocateId(), "same", {});
    const auto third = world.createRobot(manager.allocateId(), "same", {});
    ASSERT_NE(first, kNullEntity);
    EXPECT_NE(second, third);
    EXPECT_FALSE(world.registry().all_of<NameComponent>(first));
    EXPECT_TRUE(world.registry().all_of<RandomMovementComponent>(first));
    EXPECT_EQ(world.createRobot(firstId, "duplicate ID", {}), kNullEntity);
    world.setName(first, "same");
    EXPECT_EQ(world.findActorsByName("same").size(), 3u);
    world.setName(first, "");
    EXPECT_FALSE(world.registry().all_of<NameComponent>(first));
    world.destroyEntity(first);
    EXPECT_FALSE(world.registry().valid(first));
    EXPECT_EQ(world.getEntity(firstId), kNullEntity);
    EXPECT_FALSE(world.destroyActor(firstId));
    const auto secondId = world.registry().get<ActorComponent>(second).id;
    EXPECT_TRUE(world.destroyActor(secondId));
    EXPECT_EQ(world.getEntity(secondId), kNullEntity);
    EXPECT_GT(manager.allocateId(), secondId);
}

TEST(ActorWorldTest, InvalidNamesPreserveExistingNamesAndDoNotCreateEntities) {
    ActorWorld world;
    const auto name = repeated("\xe4\xb8\xad", kMaxNameCharacters);
    const auto actor = world.createRobot(1, name, {});
    ASSERT_NE(actor, entt::entity(entt::null));
    EXPECT_EQ(world.registry().get<NameComponent>(actor).name, name);
    EXPECT_FALSE(world.setName(actor, std::string(kMaxNameCharacters + 1, 'a')));
    EXPECT_FALSE(world.setName(actor, "bad\nname"));
    EXPECT_EQ(world.registry().get<NameComponent>(actor).name, name);
    const auto renamed = repeated("\xf0\x9f\x98\x80", kMaxNameCharacters);
    EXPECT_TRUE(world.setName(actor, renamed));
    EXPECT_EQ(world.registry().get<NameComponent>(actor).name, renamed);
    EXPECT_TRUE(world.setName(actor, ""));
    EXPECT_FALSE(world.registry().all_of<NameComponent>(actor));
    EXPECT_EQ(world.createRobot(2, name + "a", {}), entt::entity(entt::null));
    EXPECT_EQ(world.createLocalPlayer(2, name + "a", 7, {}, PlayerMode::Survival), entt::entity(entt::null));
    EXPECT_EQ(world.getEntity(2), entt::entity(entt::null));
}

}  // namespace
