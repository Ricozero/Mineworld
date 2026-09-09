#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

#include "chunk_render_data.h"

struct Frustum {
    glm::vec4 planes[6];  // left, right, bottom, top, near, far

    static Frustum fromViewProjection(const float* viewProjection);
    bool testAABB(glm::vec3 min, glm::vec3 max) const;
    bool operator==(const Frustum&) const = default;
};

class ChunkCuller {
public:
    static constexpr size_t MAX_GRID_CELLS = size_t{1} << 21;

    void cull(const ChunkRenderView& data, const Frustum& frustum, glm::vec3 cameraPosition);
    std::span<const uint32_t> visibleChunkIndices() const { return visible_; }

private:
    struct Cell {
        uint32_t chunkIndex = std::numeric_limits<uint32_t>::max();
        uint32_t visitStamp = 0;
        ChunkFaceConnectivity connectivity{};
        uint8_t pendingExits = 0;
        uint8_t propagatedExits = 0;
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

    void rebuildGrid(std::span<const DrawableChunk> chunks);
    void refreshConnectivity(std::span<const DrawableChunk> chunks);
    void rebuildReachability(std::span<const DrawableChunk> chunks, glm::ivec3 cameraChunk);
    void rebuildCandidates(std::span<const DrawableChunk> chunks);
    void filterFrustum(std::span<const DrawableChunk> chunks, const Frustum& frustum);

    bool initialized_ = false;
    bool gridValid_ = false;
    uint32_t visitStamp_ = 0;

    Grid grid_;
    std::vector<uint32_t> fillQueue_;
    std::vector<uint32_t> reachable_;
    std::vector<uint32_t> candidates_;
    std::vector<uint32_t> visible_;

    uint64_t layoutRevision_ = 0;
    uint64_t topologyRevision_ = 0;
    uint64_t drawableRevision_ = 0;
    glm::ivec3 cameraChunk_{0};
    Frustum frustum_{};
};
