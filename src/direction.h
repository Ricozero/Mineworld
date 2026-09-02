#pragma once

#include <cmath>
#include <glm/glm.hpp>

struct Direction {
    static glm::vec3 yawForward(float yawDegrees) {
        const float yaw = glm::radians(yawDegrees);
        return glm::normalize(glm::vec3(std::cos(yaw), 0.0f, std::sin(yaw)));
    }

    static glm::vec3 yawRight(float yawDegrees) {
        return glm::normalize(glm::cross(yawForward(yawDegrees), glm::vec3(0.0f, 1.0f, 0.0f)));
    }

    static glm::vec3 lookForward(float yawDegrees, float pitchDegrees) {
        const float yaw = glm::radians(yawDegrees);
        const float pitch = glm::radians(pitchDegrees);
        return glm::normalize(glm::vec3(
            std::cos(yaw) * std::cos(pitch),
            std::sin(pitch),
            std::sin(yaw) * std::cos(pitch)));
    }

    static glm::vec3 lookRight(float yawDegrees, float pitchDegrees) {
        return glm::normalize(glm::cross(lookForward(yawDegrees, pitchDegrees), glm::vec3(0.0f, 1.0f, 0.0f)));
    }
};
