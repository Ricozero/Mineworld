#include "world.h"

#include <iostream>

#include "entity.h"
#include "system.h"

// ============ World 实现 ============

World::World() {
    initializeBlockGrid();

    // 注册系统
    registerSystem(std::make_unique<PhysicsSystem>());
    registerSystem(std::make_unique<GameLogicSystem>());
    registerSystem(std::make_unique<LifeSystem>());
    registerSystem(std::make_unique<RenderSystem>());
}

World::~World() = default;

void World::initializeBlockGrid() {
    // 初始化块网格，所有位置都设为null
    for (int x = 0; x < WORLD_WIDTH; ++x) {
        for (int y = 0; y < WORLD_HEIGHT; ++y) {
            for (int z = 0; z < WORLD_DEPTH; ++z) {
                blockGrid_[x][y][z] = entt::null;
            }
        }
    }
}

entt::entity World::createEntity() { return registry_.create(); }

entt::entity World::createNamedEntity(const std::string& name) {
    auto entity = registry_.create();
    registry_.emplace<NameComponent>(entity, name);
    nameToEntityMap_[name] = entity;
    return entity;
}

entt::entity World::createPlayer(const std::string& name, glm::vec3 position) {
    auto entity = registry_.create();

    // 添加玩家组件
    registry_.emplace<PlayerComponent>(entity, name);
    registry_.emplace<Position>(entity, position);
    registry_.emplace<Velocity>(entity);
    registry_.emplace<Health>(entity, 100.0f);
    registry_.emplace<NameComponent>(entity, name);
    registry_.emplace<Renderable>(entity, glm::vec3(0.2f, 0.6f, 0.9f));  // 蓝色
    registry_.emplace<PhysicsComponent>(entity);

    nameToEntityMap_[name] = entity;

    std::cout << "[World] Player '" << name << "' created at (" << position.x
              << ", " << position.y << ", " << position.z << ")" << std::endl;

    return entity;
}

entt::entity World::createBlock(int x, int y, int z, BlockType type) {
    if (!isValidPosition(x, y, z)) {
        return entt::null;
    }

    // 如果该位置已有方块，先销毁它
    auto existing = blockGrid_[x][y][z];
    if (existing != entt::null) {
        registry_.destroy(existing);
    }

    auto entity = registry_.create();

    // 添加方块组件
    auto color = [type]() -> glm::vec3 {
        switch (type) {
        case BlockType::Stone:
            return glm::vec3(0.5f, 0.5f, 0.5f);  // 灰色
        case BlockType::Dirt:
            return glm::vec3(0.6f, 0.4f, 0.2f);  // 棕色
        case BlockType::Grass:
            return glm::vec3(0.2f, 0.8f, 0.2f);  // 绿色
        case BlockType::Wood:
            return glm::vec3(0.6f, 0.3f, 0.1f);  // 深棕色
        case BlockType::Leaves:
            return glm::vec3(0.1f, 0.6f, 0.1f);  // 深绿色
        case BlockType::Water:
            return glm::vec3(0.2f, 0.4f, 0.9f);  // 蓝色
        case BlockType::Sand:
            return glm::vec3(0.9f, 0.9f, 0.5f);  // 淡黄色
        default:
            return glm::vec3(1.0f, 1.0f, 1.0f);  // 白色（空气）
    } }();
    registry_.emplace<Renderable>(entity, color);
    registry_.emplace<GridPosition>(entity, x, z);

    // 更新快速查询网格
    blockGrid_[x][y][z] = entity;

    return entity;
}

void World::registerSystem(std::unique_ptr<BaseSystem> system) {
    systems_.push_back(std::move(system));
}

void World::update(float deltaTime) {
    for (auto& system : systems_) {
        system->update(*this, deltaTime);
    }
}

void World::render() {
    // 如果需要单独渲染，可以调用RenderSystem
    for (auto& system : systems_) {
        if (dynamic_cast<RenderSystem*>(system.get())) {
            system->update(*this, 0.0f);
            break;
        }
    }
}

void World::destroyEntity(entt::entity entity) {
    if (registry_.all_of<NameComponent>(entity)) {
        auto& nameComp = registry_.get<NameComponent>(entity);
        nameToEntityMap_.erase(nameComp.name);
    }

    registry_.destroy(entity);
}

entt::entity World::findEntityByName(const std::string& name) const {
    auto it = nameToEntityMap_.find(name);
    return (it != nameToEntityMap_.end()) ? it->second : entt::null;
}

entt::entity World::getBlockAt(int x, int y, int z) const {
    if (!isValidPosition(x, y, z)) {
        return entt::null;
    }
    return blockGrid_[x][y][z];
}

void World::setBlockAt(int x, int y, int z, BlockType type) {
    createBlock(x, y, z, type);
}

bool World::isValidPosition(int x, int y, int z) const {
    return x >= 0 && x < WORLD_WIDTH && y >= 0 && y < WORLD_HEIGHT && z >= 0 &&
           z < WORLD_DEPTH;
}

std::vector<entt::entity> World::getAllPlayers() const {
    std::vector<entt::entity> players;
    auto view = registry_.view<PlayerComponent>();
    for (auto entity : view) {
        players.push_back(entity);
    }
    return players;
}

void World::initializeDefaultWorld() {
    std::cout << "\n[World] Initializing default world (16x16x16)..."
              << std::endl;

    // 创建地面（Y=0）
    for (int x = 0; x < WORLD_WIDTH; ++x) {
        for (int z = 0; z < WORLD_DEPTH; ++z) {
            // 下层：石头
            createBlock(x, 0, z, BlockType::Stone);

            // 中层：草地
            createBlock(x, 1, z, BlockType::Grass);

            // 上层：部分树木
            if ((x + z) % 5 == 0) {
                createBlock(x, 2, z, BlockType::Wood);
                createBlock(x, 3, z, BlockType::Leaves);
            }
        }
    }

    // 创建一个小沙滩区域
    for (int x = 10; x < 14; ++x) {
        for (int z = 10; z < 14; ++z) {
            createBlock(x, 1, z, BlockType::Sand);
        }
    }

    // 创建一个小水池
    for (int x = 6; x < 10; ++x) {
        for (int z = 6; z < 10; ++z) {
            createBlock(x, 2, z, BlockType::Water);
        }
    }
}

size_t World::getPlayerCount() const {
    return registry_.storage<PlayerComponent>()->size();
}

size_t World::getTotalEntityCount() const {
    return registry_.storage<entt::entity>()->size();
}

// ========= Chunk 相关操作实现 =========
Chunk& World::getOrCreateChunk(glm::ivec3 chunkPos) {
    auto it = chunks_.find(chunkPos);
    if (it != chunks_.end()) {
        return *(it->second);
    }

    auto& chunk = chunks_[chunkPos] = std::make_unique<Chunk>(chunkPos);
    return *chunk;
}

BlockData World::getBlockInChunk(int worldX, int worldY, int worldZ) const {
    // 计算块坐标和局部坐标
    glm::ivec3 chunkPos{
        worldX < 0 ? (worldX - Chunk::SIZE + 1) / Chunk::SIZE : worldX / Chunk::SIZE,
        worldY < 0 ? (worldY - Chunk::SIZE + 1) / Chunk::SIZE : worldY / Chunk::SIZE,
        worldZ < 0 ? (worldZ - Chunk::SIZE + 1) / Chunk::SIZE : worldZ / Chunk::SIZE};

    glm::ivec3 localPos = Chunk::worldToLocal({worldX, worldY, worldZ}, chunkPos);

    auto it = chunks_.find(chunkPos);
    if (it == chunks_.end()) {
        // 如果Chunk不存在，返回默认的空方块数据
        return BlockData{BlockType::Air, BlockOrientation::North};
    }

    return it->second->getBlock(localPos.x, localPos.y, localPos.z);
}

void World::setBlockInChunk(int worldX, int worldY, int worldZ, BlockData blockData) {
    // 计算块坐标和局部坐标
    glm::ivec3 chunkPos{
        worldX < 0 ? (worldX - Chunk::SIZE + 1) / Chunk::SIZE : worldX / Chunk::SIZE,
        worldY < 0 ? (worldY - Chunk::SIZE + 1) / Chunk::SIZE : worldY / Chunk::SIZE,
        worldZ < 0 ? (worldZ - Chunk::SIZE + 1) / Chunk::SIZE : worldZ / Chunk::SIZE};

    glm::ivec3 localPos = Chunk::worldToLocal({worldX, worldY, worldZ}, chunkPos);

    auto& chunk = getOrCreateChunk(chunkPos);
    chunk.setBlock(localPos.x, localPos.y, localPos.z, blockData);
}
