#pragma once

#include <cstdlib>
#include <utility>

#include <glm/glm.hpp>

#include "log.h"

inline int ivec3DistanceSq(glm::ivec3 a, glm::ivec3 b) {
    const glm::ivec3 delta = a - b;
    return delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
}

template <typename... Args>
[[noreturn]] void crash(fmt::format_string<Args...> fmt, Args&&... args) {
    logging::critical(fmt, std::forward<Args>(args)...);
    std::abort();
}
