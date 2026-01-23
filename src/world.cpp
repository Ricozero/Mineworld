#include <iostream>
#include <algorithm>
#include "world.h"
#include "entity.h"

// 辅助函数：获取实体名称
static std::string getEntityName(const entt::registry &registry, entt::entity entity)
{
	// 优先返回NameComponent的名称
	if (registry.all_of<NameComponent>(entity))
	{
		return registry.get<NameComponent>(entity).name;
	}
	// 如果没有NameComponent但有PlayerComponent，则返回玩家名
	else if (registry.all_of<PlayerComponent>(entity))
	{
		return registry.get<PlayerComponent>(entity).name;
	}
	// 默认名称
	return "unnamed";
}

// World 实现
World::World()
{
	// 初始化默认系统
	registerSystem(std::make_unique<MovementSystem>());
	registerSystem(std::make_unique<RenderSystem>());
	registerSystem(std::make_unique<LifeSystem>());
}

entt::entity World::createEntity()
{
	return registry_.create();
}

entt::entity World::createNamedEntity(const std::string &name)
{
	auto entity = registry_.create();
	registry_.emplace<NameComponent>(entity, name);
	nameToEntityMap_[name] = entity;
	return entity;
}

entt::entity World::createPlayer(const std::string &name, glm::vec3 position)
{
	auto entity = registry_.create();

	// 添加组件
	registry_.emplace<Position>(entity, position);
	registry_.emplace<PlayerComponent>(entity, name);
	registry_.emplace<Health>(entity, 100.0f);                          // 玩家默认100血量
	registry_.emplace<Renderable>(entity, glm::vec3(0.0f, 0.0f, 1.0f)); // 蓝色表示玩家
	registry_.emplace<NameComponent>(entity, name);                     // 同时也添加通用名称组件

	// 注册名称映射
	nameToEntityMap_[name] = entity;

	return entity;
}

void World::registerSystem(std::unique_ptr<BaseSystem> system)
{
	systems_.push_back(std::move(system));
}

void World::update(float deltaTime)
{
	for (auto &system : systems_)
	{
		system->update(*this, deltaTime);
	}
}

void World::render()
{
	// 渲染系统会自动处理渲染逻辑
	// 这里可以作为入口点调用渲染系统
	for (auto &system : systems_)
	{
		if (dynamic_cast<RenderSystem *>(system.get()))
		{
			system->update(*this, 0.0f); // 渲染通常不需要时间增量
			break;
		}
	}
}

void World::destroyEntity(entt::entity entity)
{
	// 如果实体有名称组件，则从名称映射中删除
	if (registry_.all_of<NameComponent>(entity))
	{
		auto &nameComp = registry_.get<NameComponent>(entity);
		nameToEntityMap_.erase(nameComp.name);
	}

	registry_.destroy(entity);
}

entt::entity World::findEntityByName(const std::string &name) const
{
	auto it = nameToEntityMap_.find(name);
	if (it != nameToEntityMap_.end())
	{
		return it->second;
	}
	return entt::null;
}

// MovementSystem 实现
void MovementSystem::update(World &world, float deltaTime)
{
	auto &registry = world.getRegistry();

	// 更新所有同时具有位置和速度组件的实体
	auto view = registry.view<Position, Velocity>();
	for (auto entity : view)
	{
		auto &position = registry.get<Position>(entity);
		auto &velocity = registry.get<Velocity>(entity);

		// 更新位置
		position.value += velocity.value * deltaTime;
	}
}

// RenderSystem 实现
void RenderSystem::update(World &world, float deltaTime)
{
	auto &registry = world.getRegistry();

	// 渲染所有可渲染的实体
	auto view = registry.view<Renderable, Position>();
	for (auto entity : view)
	{
		auto &renderable = registry.get<Renderable>(entity);
		auto &position = registry.get<Position>(entity);

		// 统一获取实体名称的方法
		std::string name = getEntityName(registry, entity);

		std::cout << "Rendering " << name
				  << " at (" << position.value.x << ", " << position.value.y << ", " << position.value.z << ")"
				  << " with color (" << renderable.color.x << ", " << renderable.color.y << ", " << renderable.color.z << ")"
				  << std::endl;
	}
}

// LifeSystem 实现
void LifeSystem::update(World &world, float deltaTime)
{
	auto &registry = world.getRegistry();

	// 更新所有有健康组件的实体
	auto view = registry.view<Health>();
	for (auto entity : view)
	{
		auto &health = registry.get<Health>(entity);

		// 示例：每秒恢复1点生命值
		if (health.current < health.max)
		{
			health.current = std::min(health.max, health.current + 1.0f * deltaTime);
		}

		// 检查是否有玩家组件，显示玩家状态
		if (registry.all_of<PlayerComponent>(entity))
		{
			auto &player = registry.get<PlayerComponent>(entity);
			std::cout << "Player " << player.name << " HP: " << health.current << "/" << health.max << std::endl;
		}
	}
}