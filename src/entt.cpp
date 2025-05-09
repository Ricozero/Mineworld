#include <iostream>
#include <entt/entt.hpp>

struct position {
	float x;
	float y;
};

struct velocity {
	float dx;
	float dy;
};

void entt_test() {
	entt::registry registry;

	for (auto i = 0u; i < 10u; ++i) {
		const auto entity = registry.create();
		registry.emplace<position>(entity, i * 1.f, i * 1.f);
		if(i % 2 == 0) { registry.emplace<velocity>(entity, i * .1f, i * .1f); }
	}

	auto view = registry.view<const position, velocity>();

	// use a callback
	view.each([](const auto &pos, auto &vel) {
		std::cout << pos.x << ", " << pos.y << ", " << vel.dx << ", " << vel.dy << std::endl;
	});
	std::cout << std::endl;

	// use an extended callback
	view.each([](const auto entity, const auto &pos, auto &vel) {
		std::cout << pos.x << ", " << pos.y << ", " << vel.dx << ", " << vel.dy << std::endl;
	});
	std::cout << std::endl;

	// use a range-for
	for (auto [entity, pos, vel]: view.each()) {
		std::cout << pos.x << ", " << pos.y << ", " << vel.dx << ", " << vel.dy << std::endl;
	}
	std::cout << std::endl;

	// use forward iterators and get only the components of interest
	for (auto entity: view) {
		auto &vel = view.get<velocity>(entity);
		std::cout << vel.dx << ", " << vel.dy << std::endl;
	}
}
