#include <spdlog/spdlog.h>

#include <cstdlib>

template <typename... Args>
[[noreturn]] void crash(fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::critical(fmt, std::forward<Args>(args)...);
    std::abort();
}