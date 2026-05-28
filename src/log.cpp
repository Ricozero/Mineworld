#include "log.h"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <mutex>

namespace logging {
namespace {

std::shared_ptr<spdlog::logger> currentLogger_;
std::once_flag initFlag;

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

std::shared_ptr<spdlog::logger> createLogger(const char* name, const std::shared_ptr<spdlog::sinks::sink>& sink) {
    auto logger = std::make_shared<spdlog::logger>(name, sink);
    logger->set_level(spdlog::level::trace);
    spdlog::register_logger(logger);
    return logger;
}

}  // namespace

void init() {
    std::call_once(initFlag, []() {
        spdlog::set_pattern("[%n] [%^%l%$] %v");

        auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        createLogger(channelName(Channel::App), sink);
        createLogger(channelName(Channel::Client), sink);
        createLogger(channelName(Channel::Server), sink);

        currentLogger_ = spdlog::get(channelName(Channel::App));
    });
}

std::shared_ptr<spdlog::logger> getLogger(Channel channel) {
    init();
    return spdlog::get(channelName(channel));
}

std::shared_ptr<spdlog::logger> currentLogger() {
    init();
    if (!currentLogger_) {
        currentLogger_ = getLogger(Channel::App);
    }
    return currentLogger_;
}

Scope::Scope(Channel channel)
    : previous_(currentLogger()) {
    currentLogger_ = getLogger(channel);
}

Scope::~Scope() {
    currentLogger_ = previous_;
}

}  // namespace logging
