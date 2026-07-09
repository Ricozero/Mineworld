#pragma once

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace logging {

enum class Channel {
    App,
    Client,
    Server,
};

void init(const std::string& dir);
std::shared_ptr<spdlog::logger> getLogger(Channel channel);
std::shared_ptr<spdlog::logger> currentLogger();

class Scope {
public:
    explicit Scope(Channel channel);
    ~Scope();

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    std::shared_ptr<spdlog::logger> previous_;
};

template <typename... Args>
void trace(fmt::format_string<Args...> fmt, Args&&... args) {
    currentLogger()->trace(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void debug(fmt::format_string<Args...> fmt, Args&&... args) {
    currentLogger()->debug(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void info(fmt::format_string<Args...> fmt, Args&&... args) {
    currentLogger()->info(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void warn(fmt::format_string<Args...> fmt, Args&&... args) {
    currentLogger()->warn(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void error(fmt::format_string<Args...> fmt, Args&&... args) {
    currentLogger()->error(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void critical(fmt::format_string<Args...> fmt, Args&&... args) {
    currentLogger()->critical(fmt, std::forward<Args>(args)...);
}

}  // namespace logging
