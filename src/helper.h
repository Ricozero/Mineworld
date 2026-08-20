#pragma once

#include <cstdint>
#include <cstdlib>
#include <glm/glm.hpp>
#include <utility>

#include "log.h"

inline int ivec3DistanceSq(glm::ivec3 a, glm::ivec3 b) {
    const glm::ivec3 delta = a - b;
    return delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
}

inline uint32_t packColor(glm::vec3 color) {
    color = glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f));
    const uint32_t r = static_cast<uint32_t>(color.r * 255.0f);
    const uint32_t g = static_cast<uint32_t>(color.g * 255.0f);
    const uint32_t b = static_cast<uint32_t>(color.b * 255.0f);
    return 0xff000000u | (b << 16) | (g << 8) | r;
}

template <typename... Args>
[[noreturn]] void crash(fmt::format_string<Args...> fmt, Args&&... args) {
    logging::critical(fmt, std::forward<Args>(args)...);
    std::abort();
}
