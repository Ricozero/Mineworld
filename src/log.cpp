#include "log.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <filesystem>
#include <memory>
#include <vector>

namespace logging {
namespace {

std::shared_ptr<spdlog::logger> currentLogger_;

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
    spdlog::set_pattern("[%n] [%^%l%$] %v");

    std::vector<spdlog::sink_ptr> sinks{
        std::make_shared<spdlog::sinks::stdout_color_sink_mt>(),
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

Scope::Scope(Channel channel)
    : previous_(currentLogger()) {
    currentLogger_ = getLogger(channel);
}

Scope::~Scope() {
    currentLogger_ = previous_;
}

}  // namespace logging
