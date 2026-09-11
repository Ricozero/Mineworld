#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <variant>
#include <vector>

enum class CommandOperation : uint16_t {
    None,
    CreateRobot,
    DestroyRobot,
    Count,
};

enum class CommandStatus : uint8_t {
    Success,
    Failed,
    InvalidRequestId,
    InvalidPlayer,
    InvalidOperation,
    InvalidArguments,
    ObjectNotFound,
    Count,
};

using CommandArgument = std::variant<std::string, int64_t, double, bool, glm::vec3>;

struct CommandRequest {
    uint64_t requestId = 0;
    CommandOperation operation = CommandOperation::None;
    std::vector<CommandArgument> arguments;
};

struct CommandResponse {
    uint64_t requestId = 0;
    CommandStatus status = CommandStatus::Success;
};
