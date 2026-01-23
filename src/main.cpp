#include <iostream>
#include <memory>
#include "world.h"
#include "entity.h"

int main(int argc, char *argv[])
{
	World world;

	// 创建玩家
	auto player = world.createPlayer("Steve", glm::vec3(0.0f, 0.0f, 0.0f));

	// 为玩家添加速度组件，使其能够移动
	world.getRegistry().emplace<Velocity>(player, glm::vec3(1.0f, 0.0f, 0.0f));

	// 创建一些普通实体
	auto entity1 = world.createNamedEntity("Entity1");
	world.getRegistry().emplace<Position>(entity1, glm::vec3(5.0f, 0.0f, 0.0f));
	world.getRegistry().emplace<Velocity>(entity1, glm::vec3(-0.5f, 0.5f, 0.0f));
	world.getRegistry().emplace<Renderable>(entity1, glm::vec3(1.0f, 0.0f, 0.0f)); // 红色
	world.getRegistry().emplace<Health>(entity1, 50.0f);

	auto entity2 = world.createNamedEntity("Entity2");
	world.getRegistry().emplace<Position>(entity2, glm::vec3(0.0f, 5.0f, 0.0f));
	world.getRegistry().emplace<Velocity>(entity2, glm::vec3(0.0f, -1.0f, 0.0f));
	world.getRegistry().emplace<Renderable>(entity2, glm::vec3(0.0f, 1.0f, 0.0f)); // 绿色
	world.getRegistry().emplace<Health>(entity2, 75.0f);

	// 模拟游戏循环
	float deltaTime = 0.016f; // 约60FPS
	for (int frame = 0; frame < 5; ++frame)
	{
		std::cout << "\n--- Frame " << frame + 1 << " ---" << std::endl;

		// 更新世界
		world.update(deltaTime);

		// 查找并输出特定实体的信息
		auto entity1Ref = world.findEntityByName("Entity1");
		if (entity1Ref != entt::null)
		{
			auto &pos = world.getRegistry().get<Position>(entity1Ref);
			std::cout << "Entity1 position: (" << pos.value.x << ", " << pos.value.y << ", " << pos.value.z << ")" << std::endl;
		}

		// 输出总数统计
		std::cout << "Total entities with position: " << world.getRegistry().view<Position>().size() << std::endl;
		std::cout << "Total entities with health: " << world.getRegistry().view<Health>().size() << std::endl;
	}

	std::cout << "\n--- Final State ---" << std::endl;

	// 使用EnTT视图查询特定类型的组件组合
	auto &registry = world.getRegistry();

	// 查询所有既有Position又有Velocity的实体
	auto movable_view = registry.view<Position, Velocity>();
	std::cout << "Movable entities count: " << movable_view.size_hint() << std::endl;

	// 查询所有玩家
	auto player_view = registry.view<PlayerComponent, Health>();
	std::cout << "Player entities count: " << player_view.size_hint() << std::endl;

	return 0;
}