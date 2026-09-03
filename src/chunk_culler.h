#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

#include "chunk_mesh_pool.h"

struct Frustum {
    glm::vec4 planes[6];  // left, right, bottom, top, near, far

    static Frustum fromViewProjection(const float* viewProjection);
    bool testAABB(glm::vec3 min, glm::vec3 max) const;
};

class ChunkCuller {
public:
    static constexpr size_t MAX_GRID_CELLS = size_t{1} << 21;

    void cull(const std::vector<DrawableChunk>& chunks, const Frustum& frustum, glm::vec3 cameraPosition);
    const std::vector<DrawableChunk>& visibleChunks() const { return visible_; }

private:
    struct Cell {
        uint32_t stamp = 0;
        uint32_t chunkIndex = 0;
        uint8_t marks = 0;
    };

    struct Grid {
        std::vector<Cell> cells;
        glm::ivec3 origin{0};
        glm::ivec3 size{0};

        bool rebase(glm::ivec3 newMin, glm::ivec3 newMax);
        bool contains(glm::ivec3 chunkPos) const;
        size_t indexOf(glm::ivec3 chunkPos) const;
        ptrdiff_t neighborOffset(glm::ivec3 faceOffset) const;
    };

    struct FillNode {
        uint32_t cellIndex = 0;
        int8_t inFace = -1;
    };

    std::vector<DrawableChunk> visible_;
    Grid grid_;
    std::vector<FillNode> fillQueue_;
    uint32_t stamp_ = 0;
};
