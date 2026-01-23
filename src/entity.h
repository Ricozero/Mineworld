#pragma once

#include <glm/glm.hpp>
#include <string>

// 基础组件定义
struct Position
{
	glm::vec3 value;

	Position() : value(0.0f, 0.0f, 0.0f) {}
	Position(float x, float y, float z) : value(x, y, z) {}
	Position(glm::vec3 pos) : value(pos) {}
};

struct Velocity
{
	glm::vec3 value;

	Velocity() : value(0.0f, 0.0f, 0.0f) {}
	Velocity(float x, float y, float z) : value(x, y, z) {}
	Velocity(glm::vec3 vel) : value(vel) {}
};

struct PlayerComponent
{
	std::string name;
	float health = 100.0f;
	float speed = 1.0f;

	explicit PlayerComponent(const std::string &n) : name(n) {}
};

struct NameComponent
{
	std::string name;

	NameComponent() = default;
	explicit NameComponent(const std::string &n) : name(n) {}
};

struct Renderable
{
	glm::vec3 color;
	float size = 1.0f;

	Renderable() : color(1.0f, 1.0f, 1.0f) {}
	Renderable(float r, float g, float b) : color(r, g, b) {}
	Renderable(glm::vec3 c) : color(c) {}
};

struct Health
{
	float current;
	float max;

	Health(float max_health = 100.0f) : current(max_health), max(max_health) {}
};