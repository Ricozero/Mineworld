#include "text.h"

#include <algorithm>

std::string_view trimText(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

std::optional<size_t> utf8CodepointCount(std::string_view text) {
    size_t count = 0;
    for (size_t i = 0; i < text.size();) {
        ++count;
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        unsigned remaining = 0;
        uint32_t codepoint = 0;
        uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            remaining = 1;
            codepoint = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            remaining = 2;
            codepoint = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            remaining = 3;
            codepoint = first & 0x07;
            minimum = 0x10000;
        } else return std::nullopt;
        if (i + remaining > text.size()) return std::nullopt;
        while (remaining--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return std::nullopt;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return std::nullopt;
    }
    return count;
}

bool isValidUtf8(std::string_view text) {
    return utf8CodepointCount(text).has_value();
}

bool appendUtf8Codepoint(std::string& text, uint32_t codepoint, size_t maxBytes) {
    if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
    // clang-format off
    const size_t length = codepoint < 0x80 ? 1 : codepoint < 0x800 ? 2 : codepoint < 0x10000 ? 3 : 4;
    // clang-format on
    if (text.size() > maxBytes || length > maxBytes - text.size()) return false;
    char utf8[4];
    for (size_t i = length - 1; i > 0; --i) {
        utf8[i] = static_cast<char>(0x80 | (codepoint & 0x3f));
        codepoint >>= 6;
    }
    constexpr uint32_t kPrefixes[] = {0, 0, 0xc0, 0xe0, 0xf0};
    utf8[0] = static_cast<char>(kPrefixes[length] | codepoint);
    text.append(utf8, length);
    return true;
}

size_t previousUtf8CodepointOffset(std::string_view text, size_t offset) {
    offset = std::min(offset, text.size());
    if (offset == 0) return 0;
    --offset;
    while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) --offset;
    return offset;
}

size_t nextUtf8CodepointOffset(std::string_view text, size_t offset) {
    if (offset >= text.size()) return text.size();
    ++offset;
    while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) ++offset;
    return offset;
}

bool isSingleLineText(std::string_view text, size_t maxBytes) {
    if (text.size() > maxBytes || !isValidUtf8(text)) return false;
    for (unsigned char c : text) {
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

bool isValidName(std::string_view name) {
    if (name.size() > kMaxNameCharacters * 4) return false;
    const auto length = utf8CodepointCount(name);
    if (!length || *length > kMaxNameCharacters) return false;
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}
