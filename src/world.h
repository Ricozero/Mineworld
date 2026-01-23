#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>

// 前向声明
class BaseSystem;

// 世界管理器 - 基于EnTT的ECS核心
class World
{
public:
	World();
	~World() = default;

	// 创建实体
	entt::entity createEntity();

	// 创建带名称的实体
	entt::entity createNamedEntity(const std::string &name);

	// 创建玩家
	entt::entity createPlayer(const std::string &name, glm::vec3 position = glm::vec3(0.0f));

	// 注册系统
	void registerSystem(std::unique_ptr<BaseSystem> system);

	// 更新所有系统
	void update(float deltaTime);

	// 渲染（如果有渲染系统）
	void render();

	// 获取注册表引用
	entt::registry &getRegistry() { return registry_; }
	const entt::registry &getRegistry() const { return registry_; }

	// 销毁实体
	void destroyEntity(entt::entity entity);

	// 实体查找
	entt::entity findEntityByName(const std::string &name) const;

private:
	entt::registry registry_;
	std::vector<std::unique_ptr<BaseSystem>> systems_;
	std::unordered_map<std::string, entt::entity> nameToEntityMap_;
};

// 基础系统接口
class BaseSystem
{
public:
	virtual ~BaseSystem() = default;
	virtual void update(World &world, float deltaTime) = 0;
};

// 移动系统 - 处理带有Position和Velocity组件的实体
class MovementSystem : public BaseSystem
{
public:
	void update(World &world, float deltaTime) override;
};

// 渲染系统 - 处理带有Renderable组件的实体
class RenderSystem : public BaseSystem
{
public:
	void update(World &world, float deltaTime) override;
};

// 生命周期系统 - 处理实体健康值等
class LifeSystem : public BaseSystem
{
public:
	void update(World &world, float deltaTime) override;
};
