#if defined(_WIN32)

#include <gtest/gtest.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

#include "log.h"
#include "stdin_console.h"
#include "text.h"

namespace {

bool runTerminalWorker() {
    if (std::wstring_view(GetCommandLineW()).find(L"--stdin-console-worker") != std::wstring_view::npos) return true;
    std::wstring executable(32768, L'\0');
    executable.resize(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    const std::string filter = std::string(info->test_suite_name()) + "." + info->name();
    const auto report = std::filesystem::current_path() / ("stdin-console-" + std::to_string(GetCurrentProcessId()) + ".xml");
    std::wstring command = L"\"" + executable + L"\" --stdin-console-worker --gtest_filter=" + std::wstring(filter.begin(), filter.end()) + L" \"--gtest_output=xml:" + report.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &startup, &process)) {
        ADD_FAILURE() << "Cannot create isolated console: " << GetLastError();
        return false;
    }
    const auto wait = WaitForSingleObject(process.hProcess, 15000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::string details;
    if (exitCode != 0) {
        std::ifstream file(report);
        details.assign(std::istreambuf_iterator<char>(file), {});
    }
    std::error_code error;
    std::filesystem::remove(report, error);
    EXPECT_EQ(wait, WAIT_OBJECT_0) << "Terminal test timed out";
    EXPECT_EQ(exitCode, 0u) << details;
    return false;
}

void sendConsoleKey(StdinConsole& console, WORD key, wchar_t character = 0, WORD repeats = 1) {
    INPUT_RECORD record{};
    record.EventType = KEY_EVENT;
    record.Event.KeyEvent.bKeyDown = TRUE;
    record.Event.KeyEvent.wVirtualKeyCode = key;
    record.Event.KeyEvent.uChar.UnicodeChar = character;
    record.Event.KeyEvent.wRepeatCount = repeats;
    DWORD count = 0;
    ASSERT_TRUE(WriteConsoleInputW(GetStdHandle(STD_INPUT_HANDLE), &record, 1, &count));
    ASSERT_EQ(count, 1u);
    console.poll();
}

void sendConsoleText(StdinConsole& console, std::wstring_view text) {
    for (wchar_t c : text) {
        ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, 0, c));
    }
}

void expectConsoleLine(StdinConsole& console, std::string_view expected) {
    const auto line = console.consumeLine();
    ASSERT_TRUE(line);
    EXPECT_TRUE(line->error.empty()) << line->error;
    EXPECT_EQ(line->text, expected);
}

std::wstring currentConsoleRow() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) return {};
    std::wstring row(info.dwSize.X, L'\0');
    DWORD count = 0;
    if (!ReadConsoleOutputCharacterW(GetStdHandle(STD_OUTPUT_HANDLE), row.data(), static_cast<DWORD>(row.size()), {0, info.dwCursorPosition.Y}, &count)) return {};
    row.resize(count);
    const auto last = row.find_last_not_of(L' ');
    return last == std::wstring::npos ? L"" : row.substr(0, last + 1);
}

int currentConsoleColumn() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) return -1;
    return info.dwCursorPosition.X;
}

TEST(StdinTerminalTest, LeftRightInsertAndBackspaceAtCursor) {
    if (!runTerminalWorker()) return;
    StdinConsole console;
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"ac"));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_LEFT));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"b"));
    EXPECT_EQ(currentConsoleRow(), L"> abc");
    EXPECT_EQ(currentConsoleColumn(), 4);
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_RIGHT));
    EXPECT_EQ(currentConsoleColumn(), 5);
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_LEFT, 0, 128));
    EXPECT_EQ(currentConsoleColumn(), 2);
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"\bX"));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_RIGHT, 0, 128));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"Z\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "XabcZ"));

    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"abcd"));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_LEFT, 0, 2));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"\bY\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "aYcd"));
}

TEST(StdinTerminalTest, CursorSkipsSurrogatePairsAndDeletesWholeCodepoint) {
    if (!runTerminalWorker()) return;
    StdinConsole console;
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"A\xd83d\xde00" L"B"));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_LEFT, 0, 2));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"x"));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_RIGHT));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"\b\xd83d\xde00\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "Ax\xf0\x9f\x98\x80" "B"));
}

TEST(StdinTerminalTest, CursorPositionSurvivesLogsAndPreservesHistoryDraft) {
    if (!runTerminalWorker()) return;
    logging::init(".");
    StdinConsole console;
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"abcd\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "abcd"));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_UP));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_LEFT, 0, 2));
    EXPECT_EQ(currentConsoleColumn(), 4);
    logging::info("Message during cursor editing");
    EXPECT_EQ(currentConsoleColumn(), 4);
    EXPECT_EQ(currentConsoleRow(), L"> abcd");
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"X"));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_UP));
    EXPECT_EQ(currentConsoleRow(), L"> abcd");
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_DOWN));
    EXPECT_EQ(currentConsoleRow(), L"> abXcd");
    EXPECT_EQ(currentConsoleColumn(), 7);
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_LEFT, 0, 2));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"Y\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "abXYcd"));

    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"draft"));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_UP));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_LEFT));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_DOWN));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "draft"));
}

TEST(StdinTerminalTest, LongInputViewportFollowsCursor) {
    if (!runTerminalWorker()) return;
    StdinConsole console;
    CONSOLE_SCREEN_BUFFER_INFO info{};
    ASSERT_TRUE(GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info));
    const size_t width = static_cast<size_t>(info.srWindow.Right - info.srWindow.Left + 1);
    ASSERT_LT(width * 3, MAX_INPUT_TEXT_BYTES - 16);
    const std::wstring text = L"START" + std::wstring(width * 3, L'x') + L"END";
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, text));
    EXPECT_TRUE(currentConsoleRow().ends_with(L"END"));
    for (size_t i = 0; i < text.size(); i += 128) {
        ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_LEFT, 0, 128));
    }
    EXPECT_TRUE(currentConsoleRow().starts_with(L"> START"));
    EXPECT_EQ(currentConsoleColumn(), 2);
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"X"));
    for (size_t i = 0; i < text.size(); i += 128) {
        ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_RIGHT, 0, 128));
    }
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"Y\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "XSTART" + std::string(width * 3, 'x') + "ENDY"));
}

TEST(StdinTerminalTest, HistoryRestoresDraftCopiesEditsAndClearsDraft) {
    if (!runTerminalWorker()) return;
    StdinConsole console;
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"first\rsecond\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "first"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "second"));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"draft"));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_UP, 0, 2));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_DOWN, 0, 2));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "draft"));

    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_UP, 0, 2));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"!"));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_UP));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_DOWN));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "second!"));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_UP, 0, 3));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "second"));

    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"discard"));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_DOWN));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, ""));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_UP));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "second"));
}

TEST(StdinTerminalTest, HistoryPreservesUnicodeAndOverflowDraft) {
    if (!runTerminalWorker()) return;
    StdinConsole console;
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"help\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "help"));
    for (unsigned i = 0; i < 32; ++i) {
        ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, 0, L'a', 128));
    }
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"a"));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_UP));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_DOWN));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"\r"));
    const auto invalid = console.consumeLine();
    ASSERT_TRUE(invalid);
    EXPECT_FALSE(invalid->error.empty());
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_UP));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "help"));

    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"\xd83d\xde00\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "\xf0\x9f\x98\x80"));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_UP));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"\bn\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "n"));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_UP, 0, 2));
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "\xf0\x9f\x98\x80"));
}

TEST(StdinTerminalTest, RedrawClearsLongHistoryAndSurvivesLogging) {
    if (!runTerminalWorker()) return;
    logging::init(".");
    StdinConsole console;
    const std::wstring longText(256, L'x');
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, longText + L"\rhi\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, std::string(256, 'x')));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "hi"));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_UP, 0, 2));
    ASSERT_NO_FATAL_FAILURE(sendConsoleKey(console, VK_DOWN));
    EXPECT_EQ(currentConsoleRow(), L"> hi");
    std::jthread logger([] {
        logging::setThreadChannel(logging::Channel::Server);
        for (unsigned i = 0; i < 16; ++i) logging::info("Background message {}", i);
    });
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"!"));
    logger.join();
    EXPECT_EQ(currentConsoleRow(), L"> hi!");
    ASSERT_NO_FATAL_FAILURE(sendConsoleText(console, L"\r"));
    ASSERT_NO_FATAL_FAILURE(expectConsoleLine(console, "hi!"));
}

}  // namespace

#endif
