#include <gtest/gtest.h>

#include <array>
#include <limits>

#include "test_support.h"
#include "text.h"

namespace {

using namespace test_support;

TEST(TextTest, CursorOffsetsFollowUtf8Codepoints) {
    const std::string_view text = "a\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80z";
    const std::array<size_t, 6> boundaries{0, 1, 3, 6, 10, 11};
    for (size_t i = 1; i < boundaries.size(); ++i) {
        EXPECT_EQ(nextUtf8CodepointOffset(text, boundaries[i - 1]), boundaries[i]);
        EXPECT_EQ(previousUtf8CodepointOffset(text, boundaries[i]), boundaries[i - 1]);
    }
    EXPECT_EQ(previousUtf8CodepointOffset(text, 0), 0u);
    EXPECT_EQ(nextUtf8CodepointOffset(text, text.size()), text.size());
    EXPECT_EQ(nextUtf8CodepointOffset(text, std::numeric_limits<size_t>::max()), text.size());
    EXPECT_EQ(previousUtf8CodepointOffset(text, std::numeric_limits<size_t>::max()), 10u);
    EXPECT_EQ(previousUtf8CodepointOffset({}, 1), 0u);
    EXPECT_EQ(nextUtf8CodepointOffset({}, 0), 0u);
}

TEST(TextTest, CursorOffsetsHandlePartialInput) {
    const std::string_view text = "a\xf0\x9f\x98\x80";
    for (size_t size = 2; size <= text.size(); ++size) {
        const auto partial = text.substr(0, size);
        EXPECT_EQ(nextUtf8CodepointOffset(partial, 1), size);
        EXPECT_EQ(previousUtf8CodepointOffset(partial, size), 1u);
    }
}

TEST(TextTest, AppendsUtf8AcrossEncodingBoundaries) {
    const struct {
        uint32_t codepoint;
        std::string_view bytes;
    } cases[] = {
        {0, std::string_view("\0", 1)},
        {0x7f, "\x7f"},
        {0x80, "\xc2\x80"},
        {0x7ff, "\xdf\xbf"},
        {0x800, "\xe0\xa0\x80"},
        {0xd7ff, "\xed\x9f\xbf"},
        {0xe000, "\xee\x80\x80"},
        {0xffff, "\xef\xbf\xbf"},
        {0x10000, "\xf0\x90\x80\x80"},
        {0x10ffff, "\xf4\x8f\xbf\xbf"},
    };
    for (const auto& entry : cases) {
        std::string text = "prefix";
        ASSERT_TRUE(appendUtf8Codepoint(text, entry.codepoint, text.size() + entry.bytes.size()));
        EXPECT_EQ(text, "prefix" + std::string(entry.bytes));
        EXPECT_TRUE(isValidUtf8(text));
    }
}

TEST(TextTest, RejectsInvalidCodepointsAndLimitsWithoutPartialAppend) {
    std::string text = "prefix";
    for (uint32_t codepoint : {0xd800u, 0xdfffu, 0x110000u, std::numeric_limits<uint32_t>::max()}) {
        EXPECT_FALSE(appendUtf8Codepoint(text, codepoint, 64));
        EXPECT_EQ(text, "prefix");
    }
    for (size_t limit : {size_t(0), text.size() - 1, text.size(), text.size() + 2}) {
        EXPECT_FALSE(appendUtf8Codepoint(text, 0x4e2d, limit));
        EXPECT_EQ(text, "prefix");
    }
    EXPECT_TRUE(appendUtf8Codepoint(text, 0x4e2d, text.size() + 3));
    EXPECT_EQ(text, "prefix\xe4\xb8\xad");
}

TEST(NameTest, CountsCodepointsAndRejectsInvalidText) {
    EXPECT_TRUE(isValidName(""));
    EXPECT_EQ(utf8CodepointCount(""), 0u);
    for (std::string_view character : {"a", "\xe4\xb8\xad", "\xf0\x9f\x98\x80"}) {
        const auto name = repeated(character, kMaxNameCharacters);
        EXPECT_EQ(utf8CodepointCount(name), kMaxNameCharacters);
        EXPECT_TRUE(isValidName(name));
        EXPECT_FALSE(isValidName(name + std::string(character)));
    }
    EXPECT_EQ(utf8CodepointCount("e\xcc\x81"), 2u);
    for (std::string_view name : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe4\xb8"}) {
        EXPECT_FALSE(utf8CodepointCount(name));
        EXPECT_FALSE(isValidName(name));
    }
    EXPECT_FALSE(isValidName("bad\nname"));
    EXPECT_FALSE(isValidName(std::string_view("a\0b", 3)));
}

}  // namespace
