#pragma once

#define GLM_ENABLE_EXPERIMENTAL

#include <array>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

#include "block.h"
#include "chunk.h"

class BaseSystem;

// 世界常量
constexpr int WORLD_WIDTH = 16;   // X轴：16个方块
constexpr int WORLD_HEIGHT = 16;  // Y轴：16个方块
constexpr int WORLD_DEPTH = 16;   // Z轴：16个方块

// 世界管理器 - 基于EnTT的ECS核心
class World {
public:
    World();
    ~World();

    // ========== 实体创建 ==========
    // 创建基础实体
    entt::entity createEntity();

    // 创建带名称的实体
    entt::entity createNamedEntity(const std::string& name);

    // 创建玩家实体
    entt::entity createPlayer(const std::string& name,
                              glm::vec3 position = glm::vec3(0.0f));

    // 创建方块实体（在特定Chunk位置）
    entt::entity createBlock(int x, int y, int z, BlockType type);

    // ========== 系统管理 ==========
    void registerSystem(std::unique_ptr<BaseSystem> system);
    void update(float deltaTime);
    void render();

    // ========== 访问器 ==========
    entt::registry& getRegistry() { return registry_; }
    const entt::registry& getRegistry() const { return registry_; }

    // ========== 实体操作 ==========
    void destroyEntity(entt::entity entity);
    entt::entity findEntityByName(const std::string& name) const;

    // ========== 世界查询 ==========
    // 获取指定位置的方块
    entt::entity getBlockAt(int x, int y, int z) const;

    // 设置指定位置的方块
    void setBlockAt(int x, int y, int z, BlockType type);

    // 检查坐标是否有效
    bool isValidPosition(int x, int y, int z) const;

    // 获取所有玩家
    std::vector<entt::entity> getAllPlayers() const;

    // 初始化默认世界（生成方块）
    void initializeDefaultWorld();

    // 获取统计信息
    size_t getPlayerCount() const;
    size_t getTotalEntityCount() const;

    // ========= Chunk 相关操作 =========
    // 获取或创建一个Chunk
    Chunk& getOrCreateChunk(glm::ivec3 chunkPos);

    // 获取指定Chunk中的方块数据
    BlockData getBlockInChunk(int worldX, int worldY, int worldZ) const;

    // 在指定Chunk位置设置方块数据
    void setBlockInChunk(int worldX, int worldY, int worldZ, BlockData blockData);

private:
    entt::registry registry_;
    std::vector<std::unique_ptr<BaseSystem>> systems_;
    std::unordered_map<std::string, entt::entity> nameToEntityMap_;

    // 快速查询：(x,y,z) -> entity
    std::array<std::array<std::array<entt::entity, WORLD_DEPTH>, WORLD_HEIGHT>,
               WORLD_WIDTH>
        blockGrid_;

    // 存储chunks的容器
    std::unordered_map<glm::ivec3, std::unique_ptr<Chunk>> chunks_;

    // 初始化块网格
    void initializeBlockGrid();
};
