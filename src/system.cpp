#include "system.h"

#include <iomanip>
#include <iostream>

#include "entity.h"
#include "world.h"

void PhysicsSystem::update(World& world, float deltaTime) {
    applyGravity(world.getRegistry(), deltaTime);
    updateMovement(world.getRegistry(), deltaTime);
}

void PhysicsSystem::applyGravity(entt::registry& registry, float deltaTime) {
    // 对所有有物理组件的实体应用重力
    auto view = registry.view<PhysicsComponent, Velocity, Position>();
    constexpr float gravity = 9.8f;

    for (auto entity : view) {
        auto& physics = registry.get<PhysicsComponent>(entity);
        auto& velocity = registry.get<Velocity>(entity);

        if (physics.gravity) {
            // 对Y轴应用向下的重力
            velocity.value.y -= gravity * deltaTime;

            // 限制最大下落速度
            if (velocity.value.y < -20.0f) {
                velocity.value.y = -20.0f;
            }
        }
    }
}

void PhysicsSystem::updateMovement(entt::registry& registry, float deltaTime) {
    // 更新所有有位置和速度的实体
    auto view = registry.view<Position, Velocity>();

    for (auto entity : view) {
        auto& position = registry.get<Position>(entity);
        auto& velocity = registry.get<Velocity>(entity);

        // 更新位置
        position.value += velocity.value * deltaTime;
    }
}

bool PhysicsSystem::checkCollision(const glm::vec3& position) {
    // 简单的碰撞检测（可以扩展）
    return true;
}

void RenderSystem::update(World& world, float deltaTime) {
    static int renderCount = 0;
    renderCount++;

    // 每10帧输出一次统计信息，避免过多输出
    if (renderCount % 10 != 0) return;

    auto& registry = world.getRegistry();

    // 统计显示
    std::cout << "\n[Render Frame " << renderCount << "]" << std::endl;

    // 显示所有玩家状态
    auto playerView = registry.view<PlayerComponent, Position, Health>();
    std::cout << "Players (" << playerView.size_hint() << "):" << std::endl;

    for (auto entity : playerView) {
        auto& player = registry.get<PlayerComponent>(entity);
        auto& position = registry.get<Position>(entity);
        auto& health = registry.get<Health>(entity);

        std::cout << "  - " << player.name << " at (" << std::fixed
                  << std::setprecision(2) << position.value.x << ", "
                  << position.value.y << ", " << position.value.z
                  << ") HP: " << health.current << "/" << health.max
                  << std::endl;
    }

    // 显示世界统计
    std::cout << "\nWorld Statistics:" << std::endl;
    std::cout << "  - Total Entities: " << world.getTotalEntityCount()
              << std::endl;
}

void RenderSystem::renderEntity(const std::string& name,
                                const glm::vec3& position,
                                const glm::vec3& color) {
    // 这里可以输出渲染信息
    std::cout << "Rendering entity " << name << " at (" << position.x << ", "
              << position.y << ", " << position.z << ")" << std::endl;
}

void RenderSystem::renderBlock(const glm::ivec3& pos, BlockType type) {
    // 这里可以输出方块渲染信息
}

// ============ 生命周期系统实现 ============

void LifeSystem::update(World& world, float deltaTime) {
    updateHealth(world.getRegistry(), deltaTime);
}

void LifeSystem::updateHealth(entt::registry& registry, float deltaTime) {
    auto view = registry.view<Health>();

    for (auto entity : view) {
        auto& health = registry.get<Health>(entity);

        // 缓慢恢复生命值
        if (health.current < health.max) {
            health.current =
                std::min(health.max, health.current + 5.0f * deltaTime);
        }
    }
}

void GameLogicSystem::update(World& world, float deltaTime) {
    updatePlayerInput(world.getRegistry(), deltaTime);
    checkWorldBoundaries(world.getRegistry());
}

void GameLogicSystem::updatePlayerInput(entt::registry& registry,
                                        float deltaTime) {
    // 模拟简单的玩家输入处理
    // 在实际游戏中，这里会处理真实的键盘输入
    auto view = registry.view<PlayerComponent, Position, Velocity>();

    for (auto entity : view) {
        auto& player = registry.get<PlayerComponent>(entity);
        auto& velocity = registry.get<Velocity>(entity);

        // 模拟移动（可以根据输入调整）
        // 这里只是示例，实际应该根据输入来改变速度
    }
}

void GameLogicSystem::checkWorldBoundaries(entt::registry& registry) {
    // 检查玩家是否超出世界边界
    auto view = registry.view<PlayerComponent, Position>();

    for (auto entity : view) {
        auto& position = registry.get<Position>(entity);

        // 限制玩家在世界范围内
        position.value.x =
            glm::clamp(position.value.x, 0.0f, (float)WORLD_WIDTH);
        position.value.z =
            glm::clamp(position.value.z, 0.0f, (float)WORLD_DEPTH);

        // 防止玩家在世界以下
        if (position.value.y < -10.0f) {
            position.value.y = (float)WORLD_HEIGHT;  // 重置到上面
        }
    }
}