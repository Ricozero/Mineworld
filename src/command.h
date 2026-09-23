#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

inline constexpr size_t kMaxCommandArguments = 16;
inline constexpr size_t kMaxCommandStringBytes = 256;

enum class CommandOperation : uint16_t {
    None,
    Help,
    CreateRobot,
    DestroyRobot,
    Count,
};

using CommandArgument = std::variant<std::string, int64_t, double, bool, glm::vec3>;

enum class CommandArgumentType {
    String,
    Integer,
    Float,
    Bool,
    Vec3
};

struct CommandRequest {
    uint64_t requestId = 0;
    CommandOperation operation = CommandOperation::None;
    std::vector<CommandArgument> arguments;
};

struct CommandResponse {
    uint64_t requestId = 0;
    bool success = false;
    std::string message;
};

struct CommandParameter {
    std::string_view name;
    CommandArgumentType type;
    bool optional = false;
    size_t maxBytes = kMaxCommandStringBytes;
};

struct CommandDefinition {
    std::string_view name;
    CommandOperation operation;
    std::span<const CommandParameter> parameters;
    std::string_view description;
};

struct CommandParseResult {
    std::optional<CommandRequest> command;
    std::string error;
};

std::span<const CommandDefinition> commandDefinitions();
std::string commandUsage(const CommandDefinition& definition);
CommandParseResult parseCommandLine(std::string_view text);
std::string validateCommand(const CommandRequest& command);
CommandResponse commandHelp(std::string_view name = {});
