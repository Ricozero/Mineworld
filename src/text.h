#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

inline constexpr size_t MAX_INPUT_TEXT_BYTES = 4096;
inline constexpr size_t MAX_NAME_CHARACTERS = 16;

std::string_view trimText(std::string_view text);
std::optional<size_t> utf8CodepointCount(std::string_view text);
bool isValidUtf8(std::string_view text);
bool appendUtf8Codepoint(std::string& text, uint32_t codepoint, size_t maxBytes);
size_t previousUtf8CodepointOffset(std::string_view text, size_t offset);
size_t nextUtf8CodepointOffset(std::string_view text, size_t offset);
bool isSingleLineText(std::string_view text, size_t maxBytes);
bool isValidName(std::string_view name);
