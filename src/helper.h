#include <cstdlib>

#include "log.h"

template <typename... Args>
[[noreturn]] void crash(fmt::format_string<Args...> fmt, Args&&... args) {
    logging::critical(fmt, std::forward<Args>(args)...);
    std::abort();
}
