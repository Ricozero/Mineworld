#pragma once

#include <gtest/gtest.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace test_support {

inline void ensureLogger() {
    if (!spdlog::get("App")) {
        spdlog::register_logger(std::make_shared<spdlog::logger>("App", std::make_shared<spdlog::sinks::null_sink_mt>()));
    }
}

template <typename Step, typename Done>
void pumpUntil(Step step, Done done, std::chrono::seconds timeout = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        ASSERT_NO_FATAL_FAILURE(step());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(done()) << "Timed out while waiting for test progress";
}

inline std::string repeated(std::string_view character, size_t count) {
    std::string result;
    for (size_t i = 0; i < count; ++i) result += character;
    return result;
}

}  // namespace test_support
