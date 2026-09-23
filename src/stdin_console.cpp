#include "stdin_console.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#else
#error StdinConsole supports Windows and Linux.
#endif

#include <algorithm>
#include <deque>
#include <string_view>

#include "input_history.h"
#include "log.h"
#include "text.h"

struct StdinConsole::Impl {
#if defined(_WIN32)
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD originalMode = 0;
    DWORD type = FILE_TYPE_UNKNOWN;
    std::wstring consoleLine;
#elif defined(__linux__)
    int originalFlags = -1;
    termios originalMode{};
    bool modeChanged = false;
    bool flagsChanged = false;
    bool echoEnabled = false;
    enum class EscapeState {
        None,
        Escape,
        Sequence,
    };
    EscapeState escapeState = EscapeState::None;
    bool plainEscapeSequence = false;
#endif
    bool console = false;
    bool ended = false;
    bool overflow = false;
    bool draftOverflow = false;
    bool inputVisible = false;
    bool redrawNeeded = false;
    bool redrawAttached = false;
    size_t cursor = 0;
    InputHistory history;
    std::string line;
    std::string unread;
    size_t unreadOffset = 0;
    std::deque<StdinLine> lines;
    static constexpr size_t kMaxLines = 16;

#if defined(_WIN32)
    Impl() {
        if (!input || input == INVALID_HANDLE_VALUE) {
            ended = true;
            return;
        }
        type = GetFileType(input);
        console = GetConsoleMode(input, &originalMode) != 0;
        if (console) {
            const DWORD mode = (originalMode | ENABLE_PROCESSED_INPUT) & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
            if (!SetConsoleMode(input, mode)) ended = true;
        } else if (type != FILE_TYPE_DISK && type != FILE_TYPE_PIPE) ended = true;
        DWORD outputMode = 0;
        if (console && !ended && GetConsoleMode(output, &outputMode)) attachRedraw();
    }

    ~Impl() {
        if (redrawAttached) logging::setConsoleInputRedraw({});
        if (console) SetConsoleMode(input, originalMode);
    }

    void echo(std::wstring_view text) {
        DWORD written = 0;
        WriteConsoleW(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }

    std::optional<std::string> consoleText() const {
        if (consoleLine.empty()) return std::string{};
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, consoleLine.data(), static_cast<int>(consoleLine.size()), nullptr, 0, nullptr, nullptr);
        if (bytes == 0) return std::nullopt;
        std::string text(bytes, '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, consoleLine.data(), static_cast<int>(consoleLine.size()), text.data(), bytes, nullptr, nullptr);
        return text;
    }

    void restoreConsoleText() {
        const auto& text = history.current();
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
        consoleLine.resize(count);
        if (count > 0) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), consoleLine.data(), count);
        cursor = consoleLine.size();
    }

    size_t cursorByteOffset() const {
        if (cursor == 0) return 0;
        return static_cast<size_t>(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, consoleLine.data(), static_cast<int>(cursor), nullptr, 0, nullptr, nullptr));
    }

    void moveCursor(bool right) {
        if (right && cursor < consoleLine.size()) {
            const wchar_t c = consoleLine[cursor++];
            if (c >= 0xd800 && c <= 0xdbff && cursor < consoleLine.size() && consoleLine[cursor] >= 0xdc00 && consoleLine[cursor] <= 0xdfff) ++cursor;
        } else if (!right && cursor > 0) {
            const wchar_t c = consoleLine[--cursor];
            if (c >= 0xdc00 && c <= 0xdfff && cursor > 0 && consoleLine[cursor - 1] >= 0xd800 && consoleLine[cursor - 1] <= 0xdbff) --cursor;
        }
        redrawNeeded = true;
    }

    void eraseBeforeCursor() {
        if (cursor == 0 || overflow) return;
        const size_t end = cursor;
        moveCursor(false);
        consoleLine.erase(cursor, end - cursor);
        editHistory();
    }

    void echoText(std::string_view text) {
        if (text.empty()) return;
        const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(count, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), count);
        echo(wide);
    }

    void redrawConsole(bool visible) {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(output, &info)) return;
        const COORD start{0, info.dwCursorPosition.Y};
        if (inputVisible) {
            DWORD written = 0;
            FillConsoleOutputCharacterW(output, L' ', info.dwSize.X, start, &written);
            SetConsoleCursorPosition(output, start);
        }
        inputVisible = visible && !ended;
        if (inputVisible) {
            const auto display = displayText(static_cast<size_t>(info.srWindow.Right - info.srWindow.Left + 1));
            echoText(display.beforeCursor);
            CONSOLE_SCREEN_BUFFER_INFO cursorInfo{};
            const bool cursorKnown = GetConsoleScreenBufferInfo(output, &cursorInfo) != 0;
            echoText(display.afterCursor);
            if (cursorKnown) SetConsoleCursorPosition(output, cursorInfo.dwCursorPosition);
        }
    }
#elif defined(__linux__)
    Impl() {
        originalFlags = fcntl(STDIN_FILENO, F_GETFL);
        if (originalFlags < 0) {
            ended = true;
            return;
        }
        if ((originalFlags & O_NONBLOCK) == 0) {
            if (fcntl(STDIN_FILENO, F_SETFL, originalFlags | O_NONBLOCK) < 0) {
                ended = true;
                return;
            }
            flagsChanged = true;
        }
        console = isatty(STDIN_FILENO) == 1;
        if (console) {
            if (tcgetattr(STDIN_FILENO, &originalMode) < 0) {
                ended = true;
                return;
            }
            auto mode = originalMode;
            mode.c_lflag &= ~(ICANON | ECHO | ECHONL);
            mode.c_iflag &= ~(INLCR | IGNCR | ISTRIP | IXON);
            mode.c_iflag |= ICRNL;
            mode.c_cc[VMIN] = 1;
            mode.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &mode) < 0) {
                ended = true;
                return;
            }
            modeChanged = true;
            echoEnabled = isatty(STDOUT_FILENO) == 1;
            if (echoEnabled) attachRedraw();
        }
    }

    ~Impl() {
        if (redrawAttached) logging::setConsoleInputRedraw({});
        if (modeChanged) tcsetattr(STDIN_FILENO, TCSANOW, &originalMode);
        if (flagsChanged) fcntl(STDIN_FILENO, F_SETFL, originalFlags);
    }

    void echo(std::string_view text) {
        if (!echoEnabled) return;
        while (!text.empty()) {
            const auto count = write(STDOUT_FILENO, text.data(), text.size());
            if (count <= 0) return;
            text.remove_prefix(static_cast<size_t>(count));
        }
    }

    std::optional<std::string> consoleText() const { return line; }
    void restoreConsoleText() {
        line = history.current();
        cursor = line.size();
    }

    size_t cursorByteOffset() const { return cursor; }

    void moveCursor(bool right) {
        cursor = right ? nextUtf8CodepointOffset(line, cursor) : previousUtf8CodepointOffset(line, cursor);
        redrawNeeded = true;
    }

    void eraseBeforeCursor() {
        if (cursor == 0 || overflow) return;
        const size_t end = cursor;
        moveCursor(false);
        line.erase(cursor, end - cursor);
        editHistory();
    }

    void redrawConsole(bool visible) {
        if (inputVisible) echo("\r\x1b[2K");
        inputVisible = visible && !ended;
        if (inputVisible) {
            winsize size{};
            const size_t columns = ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 ? size.ws_col : 80;
            const auto display = displayText(columns);
            echo(display.beforeCursor);
            echo("\x1b[s");
            echo(display.afterCursor);
            echo("\x1b[u");
        }
    }
#endif

    void attachRedraw() {
        logging::setConsoleInputRedraw([this](bool visible) { redrawConsole(visible); });
        redrawAttached = true;
    }

    struct ConsoleDisplay {
        std::string beforeCursor;
        std::string afterCursor;
    };

    ConsoleDisplay displayText(size_t columns) const {
        const auto source = consoleText();
        auto text = source.value_or("[invalid Unicode]");
        size_t textCursor = source ? cursorByteOffset() : text.size();
        for (unsigned i = 0; i < 3 && !text.empty() && !isValidUtf8(text); ++i) text.pop_back();
        if (!isValidUtf8(text)) {
            text = "[invalid UTF-8]";
            textCursor = text.size();
        }
        textCursor = std::min(textCursor, text.size());
        const auto widthAt = [&](size_t offset) -> size_t {
            const unsigned char c = text[offset];
            // clang-format off
            return c == '\t' ? 4 : c < 0x80 ? 1 : 2;
            // clang-format on
        };
        const size_t budget = columns > 3 ? columns - 3 : 0;
        size_t suffixCells = 0;
        for (size_t i = textCursor; i < text.size() && suffixCells < budget / 3; i = nextUtf8CodepointOffset(text, i)) suffixCells += widthAt(i);
        const size_t beforeBudget = budget - std::min(suffixCells, budget / 3);
        size_t start = textCursor;
        size_t cells = 0;
        while (start > 0) {
            const size_t previous = previousUtf8CodepointOffset(text, start);
            const size_t width = widthAt(previous);
            if (width > beforeBudget - cells) break;
            cells += width;
            start = previous;
        }
        ConsoleDisplay display{"> ", {}};
        cells = 0;
        for (size_t i = start; i < text.size();) {
            const size_t width = widthAt(i);
            if (width > budget - cells) break;
            cells += width;
            const size_t next = nextUtf8CodepointOffset(text, i);
            auto& part = i < textCursor ? display.beforeCursor : display.afterCursor;
            if (text[i] == '\t') part += "    ";
            else part.append(text, i, next - i);
            i = next;
        }
        return display;
    }

    void editHistory() {
        if (auto text = consoleText()) history.edit(std::move(*text));
        draftOverflow = overflow;
        redrawNeeded = true;
    }

    void browseHistory(bool previous) {
        if (!consoleText()) return;
        if (previous) history.previous();
        else {
            if (history.isDraft()) draftOverflow = false;
            history.next();
        }
        restoreConsoleText();
        overflow = history.isDraft() && draftOverflow;
        redrawNeeded = true;
    }

    void finishConsoleLine() {
        if (redrawAttached) redrawConsole(false);
        const auto text = consoleText();
#if defined(_WIN32)
        if (redrawAttached) echo(L"> ");
        echo(consoleLine);
        echo(L"\r\n");
        consoleLine.clear();
#elif defined(__linux__)
        escapeState = EscapeState::None;
        if (redrawAttached) echo("> ");
        echo(line);
        echo("\n");
#endif
        if (!text || !isValidUtf8(*text)) {
            lines.push_back({{}, "Input contains invalid Unicode."});
            line.clear();
            overflow = false;
        } else {
            if (!overflow && text->size() <= kMaxInputTextBytes) {
                history.edit(*text);
                history.submit();
            }
            line = *text;
            finishLine();
        }
        history.edit({});
        cursor = 0;
        draftOverflow = false;
        redrawNeeded = true;
    }

    void finishLine() {
        overflow = overflow || line.size() > kMaxInputTextBytes;
        if (overflow) lines.push_back({{}, "Input exceeds " + std::to_string(kMaxInputTextBytes) + " bytes."});
        else lines.push_back({std::move(line), {}});
        line.clear();
        overflow = false;
    }

#if defined(_WIN32)
    void pollConsole() {
        constexpr unsigned kMaxEventsPerPoll = 128;
        constexpr unsigned kMaxKeyRepeatsPerEvent = 128;
        for (unsigned budget = 0; budget < kMaxEventsPerPoll && lines.size() < kMaxLines && !ended; ++budget) {
            DWORD available = 0;
            if (!GetNumberOfConsoleInputEvents(input, &available)) {
                ended = true;
                break;
            }
            if (available == 0) break;
            INPUT_RECORD record{};
            DWORD count = 0;
            if (!ReadConsoleInputW(input, &record, 1, &count)) {
                ended = true;
                break;
            }
            if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;
            const auto& key = record.Event.KeyEvent;
            const wchar_t c = key.uChar.UnicodeChar;
            if (key.wVirtualKeyCode == VK_UP || key.wVirtualKeyCode == VK_DOWN) {
                for (unsigned i = 0; i < std::min<unsigned>(key.wRepeatCount, kMaxKeyRepeatsPerEvent); ++i) browseHistory(key.wVirtualKeyCode == VK_UP);
                continue;
            }
            if (key.wVirtualKeyCode == VK_LEFT || key.wVirtualKeyCode == VK_RIGHT) {
                for (unsigned i = 0; i < std::min<unsigned>(key.wRepeatCount, kMaxKeyRepeatsPerEvent); ++i) moveCursor(key.wVirtualKeyCode == VK_RIGHT);
                continue;
            }
            if (c == 26 && consoleLine.empty()) {
                ended = true;
                break;
            }
            if (c == L'\r') {
                finishConsoleLine();
            } else if (c == L'\b') {
                for (unsigned i = 0; i < std::min<unsigned>(key.wRepeatCount, kMaxKeyRepeatsPerEvent); ++i) eraseBeforeCursor();
            } else if (c >= L' ' || c == L'\t') {
                const unsigned repeats = std::min<unsigned>(key.wRepeatCount, kMaxKeyRepeatsPerEvent);
                for (unsigned repeat = 0; repeat < repeats; ++repeat) {
                    if (consoleLine.size() >= kMaxInputTextBytes) {
                        overflow = true;
                        editHistory();
                        break;
                    }
                    consoleLine.insert(cursor, 1, c);
                    ++cursor;
                    editHistory();
                }
            }
        }
    }

    void readStream() {
        DWORD available = 4096;
        if (type == FILE_TYPE_PIPE) {
            if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
                ended = true;
            } else if (available == 0) return;
        }
        if (!ended) {
            char buffer[4096];
            DWORD count = 0;
            if (!ReadFile(input, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr) || count == 0) ended = true;
            else {
                unread.assign(buffer, count);
                unreadOffset = 0;
            }
        }
    }
#elif defined(__linux__)
    void readStream() {
        pollfd descriptor{STDIN_FILENO, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, 0);
        if (ready < 0) {
            if (errno != EINTR) ended = true;
            return;
        }
        if (ready == 0) return;
        if ((descriptor.revents & POLLNVAL) != 0) {
            ended = true;
            return;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) == 0) return;
        char buffer[4096];
        const auto count = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count > 0) {
            unread.assign(buffer, static_cast<size_t>(count));
            unreadOffset = 0;
        } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            ended = true;
        }
    }

    void consumeConsoleCharacter(unsigned char c) {
        if (c == '\n' || c == '\r') {
            escapeState = EscapeState::None;
            finishConsoleLine();
        } else if (c == originalMode.c_cc[VEOF] && c != _POSIX_VDISABLE) {
            if (line.empty() && !overflow) ended = true;
            else finishConsoleLine();
        } else if (escapeState != EscapeState::None) {
            if (escapeState == EscapeState::Escape && (c == '[' || c == 'O')) {
                escapeState = EscapeState::Sequence;
                plainEscapeSequence = true;
            } else if (escapeState == EscapeState::Sequence && c >= 0x40 && c <= 0x7e) {
                if (plainEscapeSequence && (c == 'A' || c == 'B')) browseHistory(c == 'A');
                else if (plainEscapeSequence && (c == 'C' || c == 'D')) moveCursor(c == 'C');
                escapeState = EscapeState::None;
            } else if (escapeState == EscapeState::Escape) escapeState = EscapeState::None;
            else plainEscapeSequence = false;
        } else if (c == 0x1b) {
            escapeState = EscapeState::Escape;
        } else if (c == '\b' || c == 0x7f || (c == originalMode.c_cc[VERASE] && c != _POSIX_VDISABLE)) {
            eraseBeforeCursor();
        } else if (c >= ' ' || c == '\t') {
            if (line.size() >= kMaxInputTextBytes) {
                overflow = true;
                editHistory();
            }
            if (!overflow) {
                line.insert(cursor, 1, static_cast<char>(c));
                ++cursor;
                editHistory();
            }
        }
    }
#endif

    void pollStream() {
        if (unreadOffset == unread.size()) {
            readStream();
            if (ended) {
                if (!line.empty() || overflow) finishLine();
                return;
            }
        }
        while (unreadOffset < unread.size() && lines.size() < kMaxLines && !ended) {
            const char c = unread[unreadOffset++];
#if defined(__linux__)
            if (console) {
                consumeConsoleCharacter(static_cast<unsigned char>(c));
                continue;
            }
#endif
            if (c == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                finishLine();
            } else if (!overflow) {
                if (line.size() >= kMaxInputTextBytes + 1) {
                    overflow = true;
                    line.clear();
                } else line += c;
            }
        }
    }

    void poll() {
        std::unique_lock lock(logging::consoleOutputMutex(), std::defer_lock);
        if (console) lock.lock();
        if (ended || lines.size() >= kMaxLines) return;
#if defined(_WIN32)
        if (console) {
            pollConsole();
        } else
#endif
            pollStream();
        if (redrawAttached && (redrawNeeded || ended)) redrawConsole(!ended);
        redrawNeeded = false;
    }

    std::optional<StdinLine> consumeLine() {
        if (lines.empty()) return std::nullopt;
        StdinLine result = std::move(lines.front());
        lines.pop_front();
        return result;
    }
};

StdinConsole::StdinConsole() : impl_(std::make_unique<Impl>()) {}
StdinConsole::~StdinConsole() = default;

void StdinConsole::poll() {
    impl_->poll();
}

std::optional<StdinLine> StdinConsole::consumeLine() {
    return impl_->consumeLine();
}
