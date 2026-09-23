#include <gtest/gtest.h>

#include <limits>

#include "command.h"

namespace {

TEST(CommandTest, DeclaredTypesQuotesAndVectorConsumption) {
    auto parsed = parseCommandLine(" /create_robot \"Test \\\"Bot\\\"\" 10 64 -20 ");
    ASSERT_TRUE(parsed.command) << parsed.error;
    EXPECT_EQ(parsed.command->operation, CommandOperation::CreateRobot);
    ASSERT_EQ(parsed.command->arguments.size(), 2u);
    EXPECT_EQ(std::get<std::string>(parsed.command->arguments[0]), "Test \"Bot\"");
    EXPECT_EQ(std::get<glm::vec3>(parsed.command->arguments[1]), glm::vec3(10, 64, -20));
    EXPECT_TRUE(validateCommand(*parsed.command).empty());
    parsed = parseCommandLine("create_robot \"\" 1 2 3");
    ASSERT_TRUE(parsed.command);
    EXPECT_TRUE(std::get<std::string>(parsed.command->arguments[0]).empty());
    auto id = parseCommandLine("destroy_robot 123");
    auto name = parseCommandLine("destroy_robot_named 123");
    ASSERT_TRUE(id.command);
    ASSERT_TRUE(name.command);
    EXPECT_EQ(std::get<int64_t>(id.command->arguments[0]), 123);
    EXPECT_EQ(std::get<std::string>(name.command->arguments[0]), "123");
    for (const auto* text : {"destroy_robot 0", "destroy_robot -1"}) {
        parsed = parseCommandLine(text);
        ASSERT_TRUE(parsed.command) << parsed.error;
        EXPECT_TRUE(validateCommand(*parsed.command).empty());
    }
}

TEST(CommandTest, RejectsMalformedTextAndTypedRequests) {
    for (const auto* text : {"/", "unknown", "create_robot \"open", "create_robot \"a\"b", "create_robot \"a\\q\"",
                             "create_robot name 1 2", "create_robot name nan 2 3", "create_robot name 1e100 2 3",
                             "create_robot name 1 2 3 4", "create_robot name +-1 2 3", "destroy_robot 1x",
                             "destroy_robot 9223372036854775808", "destroy_robot_named"}) {
        EXPECT_FALSE(parseCommandLine(text).command) << text;
    }
    EXPECT_FALSE(parseCommandLine("create_robot " + std::string(kMaxCommandStringBytes + 1, 'a')).command);
    EXPECT_FALSE(validateCommand({0, CommandOperation::CreateRobot, {int64_t(1)}}).empty());
    EXPECT_FALSE(validateCommand({0, CommandOperation::CreateRobot, {std::string("x"), glm::vec3(std::numeric_limits<float>::infinity())}}).empty());
    EXPECT_FALSE(validateCommand({0, CommandOperation::DestroyRobot, {false}}).empty());
    EXPECT_TRUE(validateCommand({0, CommandOperation::DestroyRobot, {int64_t(1)}}).empty());
    EXPECT_TRUE(validateCommand({0, CommandOperation::DestroyRobot, {}}).empty());
    EXPECT_FALSE(validateCommand({0, CommandOperation::None, {}}).empty());
    EXPECT_FALSE(validateCommand({0, static_cast<CommandOperation>(65535), {}}).empty());
    EXPECT_TRUE(validateCommand({0, CommandOperation::DestroyRobot, {std::string("123")}}).empty());
    EXPECT_TRUE(commandHelp().success);
    EXPECT_TRUE(commandHelp("create_robot").success);
    EXPECT_FALSE(commandHelp("unknown").success);
}

}  // namespace
