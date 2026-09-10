#pragma once

#include <cmath>
#include <glm/glm.hpp>

namespace orientation {

struct Basis {
    glm::vec3 forward;
    glm::vec3 right;
    glm::vec3 up;

    glm::vec3 transform(glm::vec3 local) const {
        return forward * local.x + up * local.y + right * local.z;
    }
};

inline Basis fromYawPitchDegrees(float yawDegrees, float pitchDegrees) {
    const float yaw = glm::radians(yawDegrees);
    const float pitch = glm::radians(pitchDegrees);
    const float cosYaw = std::cos(yaw);
    const float sinYaw = std::sin(yaw);
    const float cosPitch = std::cos(pitch);
    const float sinPitch = std::sin(pitch);

    return Basis{
        glm::vec3(cosYaw * cosPitch, sinPitch, sinYaw * cosPitch),
        glm::vec3(-sinYaw, 0.0f, cosYaw),
        glm::vec3(-cosYaw * sinPitch, cosPitch, -sinYaw * sinPitch),
    };
}

}  // namespace orientation
