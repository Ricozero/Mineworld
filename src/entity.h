#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <deque>
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
    float jumpImpulseTime = 0.0f;
    bool useGravity = true;
    bool isGrounded = false;
};

struct BoxColliderComponent {
    glm::vec3 offset{0.0f};
    glm::vec3 size{1.0f};
};

struct ControllerInputComponent {
    glm::vec3 move{0.0f};
    bool jump = false;
    bool sprint = false;
    uint32_t sequence = 0;
    float deltaTime = 0.0f;
};

enum class PlayerMode : uint8_t {
    Survival = 0,
    Spectator = 1,
};

struct PredictedInput {
    ControllerInputComponent input;
    PlayerMode playerMode = PlayerMode::Survival;
    glm::vec3 rotation{0.0f};
    float deltaTime = 0.0f;
};

struct PlayerComponent {
    PlayerMode mode = PlayerMode::Survival;
    float survivalMoveSpeed = 5.0f;
    float spectatorMoveSpeed = 10.0f;
    bool jumpRequested = false;
};

struct RobotComponent {
    float moveSpeed = 3.0f;
};

struct RandomMovementComponent {
    float changeDirectionTimer = 0.0f;
    float changeDirectionInterval = 2.0f;
    glm::vec3 targetDirection{0.0f};
};

struct MeshComponent {
    glm::vec4 color{1.0f};
    bool isVisible = true;
};

struct SessionComponent {
    uint32_t sessionId = 0;
};

struct PredictedInputComponent {
    std::deque<PredictedInput> pendingInputs;
    uint32_t nextInputSequence = 1;
    uint32_t lastAcknowledgedInputSequence = 0;
};

struct InterpolationSample {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};
    glm::vec3 velocity{0.0f};
    PlayerMode playerMode = PlayerMode::Survival;
    double time = 0.0;
};

struct InterpolationComponent {
    std::deque<InterpolationSample> samples;
};
