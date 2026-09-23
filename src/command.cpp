#include "command.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <type_traits>

#include "text.h"

namespace {

constexpr CommandParameter helpParameters[] = {
    {"command", CommandArgumentType::String, true},
};
constexpr CommandParameter createRobotParameters[] = {
    {"name", CommandArgumentType::String, true},
    {"position", CommandArgumentType::Vec3, true},
};
constexpr CommandParameter destroyRobotParameters[] = {
    {"id", CommandArgumentType::Integer, true},
};
constexpr CommandParameter destroyRobotNamedParameters[] = {
    {"name", CommandArgumentType::String, false},
};
constexpr CommandDefinition definitions[] = {
    {"help", CommandOperation::Help, helpParameters, "Show available commands or help for one command."},
    {"create_robot", CommandOperation::CreateRobot, createRobotParameters, "Create a robot; omitted position uses the player position."},
    {"destroy_robot", CommandOperation::DestroyRobot, destroyRobotParameters, "Destroy a robot by ID, or one robot if omitted."},
    {"destroy_robot_named", CommandOperation::DestroyRobot, destroyRobotNamedParameters, "Destroy a robot with the given name."},
};

const char* typeName(CommandArgumentType type) {
    switch (type) {
        case CommandArgumentType::String: return "string";
        case CommandArgumentType::Integer: return "integer";
        case CommandArgumentType::Float: return "number";
        case CommandArgumentType::Bool: return "true/false";
        case CommandArgumentType::Vec3: return "x y z";
    }
    return "unknown";
}

const CommandDefinition* findDefinition(std::string_view name) {
    for (const auto& definition : definitions) {
        if (definition.name == name) return &definition;
    }
    return nullptr;
}

bool matchesType(const CommandArgument& value, CommandArgumentType type) {
    switch (type) {
        case CommandArgumentType::String: return std::holds_alternative<std::string>(value);
        case CommandArgumentType::Integer: return std::holds_alternative<int64_t>(value);
        case CommandArgumentType::Float: return std::holds_alternative<double>(value);
        case CommandArgumentType::Bool: return std::holds_alternative<bool>(value);
        case CommandArgumentType::Vec3: return std::holds_alternative<glm::vec3>(value);
    }
    return false;
}

std::string validateSignature(const CommandDefinition& definition, const CommandRequest& command) {
    const auto required = std::count_if(definition.parameters.begin(), definition.parameters.end(), [](const auto& p) { return !p.optional; });
    if (command.arguments.size() < static_cast<size_t>(required) || command.arguments.size() > definition.parameters.size()) {
        return "Invalid argument count. Usage: " + commandUsage(definition);
    }
    for (size_t i = 0; i < command.arguments.size(); ++i) {
        const auto& parameter = definition.parameters[i];
        const auto& value = command.arguments[i];
        const std::string prefix = "Parameter '" + std::string(parameter.name) + "' ";
        if (!matchesType(value, parameter.type)) return prefix + "requires " + typeName(parameter.type) + ".";
        if (const auto* text = std::get_if<std::string>(&value)) {
            if (!isSingleLineText(*text, parameter.maxBytes)) {
                return prefix + "must be single-line UTF-8, at most " + std::to_string(parameter.maxBytes) + " bytes.";
            }
        } else if (const auto* number = std::get_if<double>(&value)) {
            if (!std::isfinite(*number)) return prefix + "must be finite.";
        } else if (const auto* position = std::get_if<glm::vec3>(&value)) {
            if (!std::isfinite(position->x) || !std::isfinite(position->y) || !std::isfinite(position->z)) return prefix + "must contain finite coordinates.";
        }
    }
    return {};
}

struct Token {
    std::string text;
    size_t column;
};

bool tokenize(std::string_view text, std::vector<Token>& tokens, std::string& error) {
    size_t cursor = 0;
    while (cursor < text.size()) {
        if (text[cursor] == ' ' || text[cursor] == '\t') {
            ++cursor;
            continue;
        }
        const size_t begin = cursor;
        std::string value;
        if (text[cursor] == '"') {
            ++cursor;
            bool closed = false;
            while (cursor < text.size()) {
                char c = text[cursor++];
                if (c == '"') {
                    closed = true;
                    break;
                }
                if (c == '\\') {
                    if (cursor == text.size() || (text[cursor] != '"' && text[cursor] != '\\')) {
                        error = "Invalid escape at column " + std::to_string(cursor) + ". Only escaped quotes and backslashes are supported.";
                        return false;
                    }
                    c = text[cursor++];
                }
                value += c;
            }
            if (!closed) {
                error = "Unclosed quote at column " + std::to_string(begin + 1) + ".";
                return false;
            }
            if (cursor < text.size() && text[cursor] != ' ' && text[cursor] != '\t') {
                error = "Expected whitespace after quote at column " + std::to_string(cursor + 1) + ".";
                return false;
            }
        } else {
            while (cursor < text.size() && text[cursor] != ' ' && text[cursor] != '\t') {
                if (text[cursor] == '"') {
                    error = "Quotes must begin an argument.";
                    return false;
                }
                value += text[cursor++];
            }
        }
        tokens.push_back({std::move(value), begin + 1});
    }
    return true;
}

template <typename Number>
bool parseNumber(std::string_view text, Number& out) {
    if (text.starts_with('+')) {
        text.remove_prefix(1);
        if (text.starts_with('-')) return false;
    }
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
    if constexpr (std::is_floating_point_v<Number>) return std::isfinite(out);
    return true;
}

}  // namespace

std::span<const CommandDefinition> commandDefinitions() { return definitions; }

std::string commandUsage(const CommandDefinition& definition) {
    std::string result = "/" + std::string(definition.name);
    for (const auto& parameter : definition.parameters) {
        result += parameter.optional ? " [" : " <";
        result += parameter.name;
        result += ":";
        result += typeName(parameter.type);
        result += parameter.optional ? "]" : ">";
    }
    return result;
}

CommandParseResult parseCommandLine(std::string_view text) {
    if (text.size() > MAX_INPUT_TEXT_BYTES || !isValidUtf8(text)) return {{}, "Command is too long or is not valid UTF-8."};
    text = trimText(text);
    if (text.starts_with('/')) text.remove_prefix(1);
    std::vector<Token> tokens;
    std::string error;
    if (!tokenize(text, tokens, error)) return {{}, std::move(error)};
    if (tokens.empty()) return {{}, "Empty command. Use /help."};
    const auto* definition = findDefinition(tokens[0].text);
    if (!definition) return {{}, "Unknown command. Use /help."};
    CommandRequest request{0, definition->operation, {}};
    size_t token = 1;
    for (const auto& parameter : definition->parameters) {
        if (token == tokens.size()) {
            if (parameter.optional) break;
            return {{}, "Missing parameter '" + std::string(parameter.name) + "'. Usage: " + commandUsage(*definition)};
        }
        const auto begin = token;
        bool valid = true;
        switch (parameter.type) {
            case CommandArgumentType::String:
                request.arguments.emplace_back(tokens[token++].text);
                break;
            case CommandArgumentType::Integer: {
                int64_t number = 0;
                valid = parseNumber(tokens[token++].text, number);
                request.arguments.emplace_back(number);
                break;
            }
            case CommandArgumentType::Float: {
                double number = 0;
                valid = parseNumber(tokens[token++].text, number);
                request.arguments.emplace_back(number);
                break;
            }
            case CommandArgumentType::Bool: {
                const auto& value = tokens[token++].text;
                valid = value == "true" || value == "false";
                request.arguments.emplace_back(value == "true");
                break;
            }
            case CommandArgumentType::Vec3: {
                glm::vec3 position{0};
                valid = tokens.size() - token >= 3;
                if (valid) {
                    for (int axis = 0; axis < 3; ++axis) valid = parseNumber(tokens[token++].text, position[axis]) && valid;
                }
                request.arguments.emplace_back(position);
                break;
            }
        }
        if (!valid) return {{}, "Parameter '" + std::string(parameter.name) + "' requires " + typeName(parameter.type) + " at column " + std::to_string(tokens[begin].column) + "."};
    }
    if (token != tokens.size()) return {{}, "Too many arguments. Usage: " + commandUsage(*definition)};
    error = validateSignature(*definition, request);
    if (!error.empty()) return {{}, std::move(error)};
    return {std::move(request), {}};
}

std::string validateCommand(const CommandRequest& command) {
    if (command.arguments.size() > MAX_COMMAND_ARGUMENTS) return "Too many command arguments.";
    std::string error;
    for (const auto& definition : definitions) {
        if (definition.operation != command.operation) continue;
        std::string candidate = validateSignature(definition, command);
        if (candidate.empty()) return {};
        bool typesMatch = command.arguments.size() <= definition.parameters.size();
        for (size_t i = 0; typesMatch && i < command.arguments.size(); ++i) typesMatch = matchesType(command.arguments[i], definition.parameters[i].type);
        if (error.empty() || typesMatch) error = std::move(candidate);
    }
    return error.empty() ? "Unknown command operation." : error;
}

CommandResponse commandHelp(std::string_view name) {
    if (name.starts_with('/')) name.remove_prefix(1);
    if (!name.empty()) {
        const auto* definition = findDefinition(name);
        if (!definition) return {0, false, "Unknown command. Use /help."};
        return {0, true, commandUsage(*definition) + "\n" + std::string(definition->description)};
    }
    std::string message;
    for (const auto& definition : definitions) {
        if (!message.empty()) message += '\n';
        message += commandUsage(definition);
    }
    return {0, true, std::move(message)};
}
