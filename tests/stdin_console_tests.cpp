#include <gtest/gtest.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "stdin_console.h"
#include "text.h"

namespace {

class StdinConsoleTest : public ::testing::Test {
protected:
    void SetUp() override {
#if defined(_WIN32)
        originalInput_ = GetStdHandle(STD_INPUT_HANDLE);
        ASSERT_TRUE(CreatePipe(&input_, &writer_, nullptr, 65536));
        ASSERT_TRUE(SetStdHandle(STD_INPUT_HANDLE, input_));
#elif defined(__linux__)
        originalInput_ = dup(STDIN_FILENO);
        ASSERT_GE(originalInput_, 0);
        int descriptors[2];
        ASSERT_EQ(pipe(descriptors), 0);
        input_ = descriptors[0];
        writer_ = descriptors[1];
        ASSERT_GE(dup2(input_, STDIN_FILENO), 0);
#endif
        inputChanged_ = true;
        console_ = std::make_unique<StdinConsole>();
    }

    void TearDown() override {
        console_.reset();
#if defined(_WIN32)
        if (inputChanged_) SetStdHandle(STD_INPUT_HANDLE, originalInput_);
        if (input_ != INVALID_HANDLE_VALUE) CloseHandle(input_);
#elif defined(__linux__)
        if (inputChanged_) dup2(originalInput_, STDIN_FILENO);
        if (originalInput_ >= 0) close(originalInput_);
        if (input_ >= 0) close(input_);
#endif
        closeWriter();
    }

    void writeInput(std::string_view text) {
        while (!text.empty()) {
#if defined(_WIN32)
            DWORD count = 0;
            ASSERT_TRUE(WriteFile(writer_, text.data(), static_cast<DWORD>(text.size()), &count, nullptr));
#elif defined(__linux__)
            const auto count = write(writer_, text.data(), text.size());
#endif
            ASSERT_GT(count, 0);
            text.remove_prefix(static_cast<size_t>(count));
        }
    }

    void closeWriter() {
#if defined(_WIN32)
        if (writer_ != INVALID_HANDLE_VALUE) CloseHandle(writer_);
        writer_ = INVALID_HANDLE_VALUE;
#elif defined(__linux__)
        if (writer_ >= 0) close(writer_);
        writer_ = -1;
#endif
    }

    std::vector<StdinLine> drain() {
        std::vector<StdinLine> result;
        for (unsigned i = 0; i < 8; ++i) {
            console_->poll();
            while (auto line = console_->consumeLine()) result.push_back(std::move(*line));
        }
        return result;
    }

    std::unique_ptr<StdinConsole> console_;

private:
    bool inputChanged_ = false;
#if defined(_WIN32)
    HANDLE originalInput_ = INVALID_HANDLE_VALUE;
    HANDLE input_ = INVALID_HANDLE_VALUE;
    HANDLE writer_ = INVALID_HANDLE_VALUE;
#elif defined(__linux__)
    int originalInput_ = -1;
    int input_ = -1;
    int writer_ = -1;
#endif
};

TEST_F(StdinConsoleTest, PollsEmptyPipeAndPreservesPartialUtf8Lines) {
    EXPECT_TRUE(drain().empty());
    ASSERT_NO_FATAL_FAILURE(writeInput("help\r"));
    EXPECT_TRUE(drain().empty());
    ASSERT_NO_FATAL_FAILURE(writeInput("\n\xe4\xb8"));
    auto lines = drain();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].text, "help");
    EXPECT_TRUE(lines[0].error.empty());
    ASSERT_NO_FATAL_FAILURE(writeInput("\xad\n\n"));
    lines = drain();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].text, "\xe4\xb8\xad");
    EXPECT_TRUE(lines[0].error.empty());
    EXPECT_TRUE(lines[1].text.empty());
    EXPECT_TRUE(lines[1].error.empty());
}

TEST_F(StdinConsoleTest, AcceptsLimitAndRecoversAfterOversizedLine) {
    const std::string maximum(MAX_INPUT_TEXT_BYTES, 'a');
    ASSERT_NO_FATAL_FAILURE(writeInput(maximum));
    EXPECT_TRUE(drain().empty());
    ASSERT_NO_FATAL_FAILURE(writeInput("\r\n"));
    auto lines = drain();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].text, maximum);
    EXPECT_TRUE(lines[0].error.empty());

    ASSERT_NO_FATAL_FAILURE(writeInput(maximum));
    EXPECT_TRUE(drain().empty());
    ASSERT_NO_FATAL_FAILURE(writeInput("more\nhelp\n"));
    lines = drain();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_TRUE(lines[0].text.empty());
    EXPECT_FALSE(lines[0].error.empty());
    EXPECT_EQ(lines[1].text, "help");
    EXPECT_TRUE(lines[1].error.empty());
}

TEST_F(StdinConsoleTest, FlushesUnterminatedLineOnlyOnceAtEof) {
    ASSERT_NO_FATAL_FAILURE(writeInput("help"));
    EXPECT_TRUE(drain().empty());
    closeWriter();
    const auto lines = drain();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].text, "help");
    EXPECT_TRUE(lines[0].error.empty());
    EXPECT_TRUE(drain().empty());
}

TEST_F(StdinConsoleTest, PreservesLinesAcrossQueueLimitAndEof) {
    std::string input;
    for (unsigned i = 0; i < 40; ++i) input += std::to_string(i) + '\n';
    ASSERT_NO_FATAL_FAILURE(writeInput(input));
    closeWriter();
    const auto lines = drain();
    ASSERT_EQ(lines.size(), 40u);
    for (unsigned i = 0; i < lines.size(); ++i) {
        EXPECT_EQ(lines[i].text, std::to_string(i));
        EXPECT_TRUE(lines[i].error.empty());
    }
    EXPECT_TRUE(drain().empty());
}

}  // namespace
