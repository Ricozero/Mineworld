#include "log.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <filesystem>
#include <memory>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace logging {
namespace {

thread_local std::shared_ptr<spdlog::logger> currentLogger_;
std::mutex consoleMutex;
std::function<void(bool)> consoleInputRedraw;

class ConsoleSink : public spdlog::sinks::sink {
public:
    void log(const spdlog::details::log_msg& message) override {
        std::lock_guard lock(consoleMutex);
        if (consoleInputRedraw) consoleInputRedraw(false);
        output_.log(message);
        if (consoleInputRedraw) {
            output_.flush();
            consoleInputRedraw(true);
        }
    }

    void flush() override {
        std::lock_guard lock(consoleMutex);
        output_.flush();
    }

    void set_pattern(const std::string& pattern) override {
        std::lock_guard lock(consoleMutex);
        output_.set_pattern(pattern);
    }

    void set_formatter(std::unique_ptr<spdlog::formatter> formatter) override {
        std::lock_guard lock(consoleMutex);
        output_.set_formatter(std::move(formatter));
    }

private:
    spdlog::sinks::stdout_color_sink_mt output_;
};

const char* channelName(Channel channel) {
    switch (channel) {
        case Channel::App:
            return "App";
        case Channel::Client:
            return "Client";
        case Channel::Server:
            return "Server";
    }
    return "App";
}

std::shared_ptr<spdlog::logger> createLogger(const char* name, const std::vector<spdlog::sink_ptr>& sinks) {
    auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::info);
    spdlog::register_logger(logger);
    return logger;
}

}  // namespace

void init(const std::string& dir) {
#if defined(_WIN32)
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD outputMode = 0;
    if (output != nullptr && output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &outputMode)) SetConsoleOutputCP(CP_UTF8);
#endif

    spdlog::set_pattern("[%n] [%^%l%$] %v");

    std::vector<spdlog::sink_ptr> sinks{
        std::make_shared<ConsoleSink>(),
        std::make_shared<spdlog::sinks::basic_file_sink_mt>((std::filesystem::path(dir) / "mineworld.log").string(), true),
    };

    createLogger(channelName(Channel::App), sinks);
    createLogger(channelName(Channel::Client), sinks);
    createLogger(channelName(Channel::Server), sinks);

    currentLogger_ = spdlog::get(channelName(Channel::App));
}

std::shared_ptr<spdlog::logger> getLogger(Channel channel) {
    return spdlog::get(channelName(channel));
}

std::shared_ptr<spdlog::logger> currentLogger() {
    if (!currentLogger_) {
        currentLogger_ = getLogger(Channel::App);
    }
    return currentLogger_;
}

void setThreadChannel(Channel channel) {
    currentLogger_ = getLogger(channel);
}

std::mutex& consoleOutputMutex() {
    return consoleMutex;
}

void setConsoleInputRedraw(std::function<void(bool)> redraw) {
    std::lock_guard lock(consoleMutex);
    if (consoleInputRedraw) consoleInputRedraw(false);
    consoleInputRedraw = std::move(redraw);
    if (consoleInputRedraw) consoleInputRedraw(true);
}

}  // namespace logging
