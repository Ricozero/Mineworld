#pragma once

#include <glm/glm.hpp>
#include <string>

struct NameComponent {
    std::string name;
};

struct TransformComponent {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};
    glm::vec3 scale{1.0f};
};

struct PhysicsComponent {
    glm::vec3 velocity{0.0f};
    glm::vec3 acceleration{0.0f};

    float mass = 1.0f;
    float drag = 0.2f;
    bool useGravity = true;
    bool isGrounded = false;
};

struct BoxColliderComponent {
    glm::vec3 offset{0.0f};
    glm::vec3 size{1.0f};
};

struct PlayerComponent {
    float moveSpeed = 5.0f;
    float jumpForce = 1.2f;
};

struct MeshComponent {
    glm::vec4 color{1.0f};
    bool isVisible = true;
};
