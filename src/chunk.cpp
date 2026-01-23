#include "chunk.h"

Chunk::Chunk(glm::ivec3 pos) : position(pos)
{
	// 初始化所有方块为空
	for (int x = 0; x < SIZE; ++x)
	{
		for (int y = 0; y < SIZE; ++y)
		{
			for (int z = 0; z < SIZE; ++z)
			{
				blocks[x][y][z] = entt::null;
			}
		}
	}
}

entt::entity Chunk::getBlock(int x, int y, int z) const
{
	if (isValidPosition(x, y, z))
	{
		return blocks[x][y][z];
	}
	return entt::null;
}

void Chunk::setBlock(int x, int y, int z, entt::entity entity)
{
	if (isValidPosition(x, y, z))
	{
		blocks[x][y][z] = entity;
	}
}

bool Chunk::isValidPosition(int x, int y, int z) const
{
	return x >= 0 && x < SIZE && y >= 0 && y < SIZE && z >= 0 && z < SIZE;
}