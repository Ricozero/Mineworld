#pragma once

#include <glm/glm.hpp>
#include <string>

struct Position {
    glm::vec3 value;

    Position() : value(0.0f, 0.0f, 0.0f) {}
    Position(float x, float y, float z) : value(x, y, z) {}
    Position(glm::vec3 pos) : value(pos) {}
};

struct Velocity {
    glm::vec3 value;

    Velocity() : value(0.0f, 0.0f, 0.0f) {}
    Velocity(float x, float y, float z) : value(x, y, z) {}
    Velocity(glm::vec3 vel) : value(vel) {}
};

struct NameComponent {
    std::string name;

    NameComponent() = default;
    explicit NameComponent(const std::string& n) : name(n) {}
};

struct PlayerComponent {
    std::string name;
    float health = 100.0f;
    float maxHealth = 100.0f;
    float speed = 5.0f;
    float jumpForce = 10.0f;

    explicit PlayerComponent(const std::string& n) : name(n) {}
};

struct Health {
    float current;
    float max;

    Health(float max_health = 100.0f) : current(max_health), max(max_health) {}
};

struct Renderable {
    glm::vec3 color;
    float size = 1.0f;

    Renderable() : color(1.0f, 1.0f, 1.0f) {}
    Renderable(float r, float g, float b) : color(r, g, b) {}
    Renderable(glm::vec3 c) : color(c) {}
};

struct PhysicsComponent {
    bool gravity = true;
    bool collision = true;
    float mass = 1.0f;
};

struct GridPosition {
    int gridX, gridZ;

    GridPosition() : gridX(0), gridZ(0) {}
    GridPosition(int gx, int gz) : gridX(gx), gridZ(gz) {}
};