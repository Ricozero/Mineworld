#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

// 区块定义 (16x16x16)
class Chunk
{
public:
	static constexpr int SIZE = 16;
	entt::entity blocks[SIZE][SIZE][SIZE];
	glm::ivec3 position; // 区块在世界中的位置

	Chunk(glm::ivec3 pos);
	~Chunk() = default;

	// 获取方块
	entt::entity getBlock(int x, int y, int z) const;
	void setBlock(int x, int y, int z, entt::entity entity);

	// 检查坐标是否在区块内
	bool isValidPosition(int x, int y, int z) const;
};
// 此文件已被移除，因为新的ECS架构不使用Chunk系统
// 新架构专注于基础ECS概念演示
