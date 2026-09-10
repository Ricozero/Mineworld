#include "chunk_culler.h"

#include <bit>
#include <cassert>
#include <glm/gtc/type_ptr.hpp>
#include <limits>

#include "chunk_mesh.h"
#include "profiler.h"

Frustum Frustum::fromViewProjection(const float* viewProjection) {
    const glm::mat4 rows = glm::transpose(glm::make_mat4(viewProjection));
    Frustum frustum;
    frustum.planes[0] = rows[3] + rows[0];
    frustum.planes[1] = rows[3] - rows[0];
    frustum.planes[2] = rows[3] + rows[1];
    frustum.planes[3] = rows[3] - rows[1];
    frustum.planes[4] = rows[3] + rows[2];
    frustum.planes[5] = rows[3] - rows[2];
    for (glm::vec4& plane : frustum.planes) {
        const float length = glm::length(glm::vec3(plane));
        if (length > 0.0f) {
            plane /= length;
        }
    }
    return frustum;
}

bool Frustum::testAABB(glm::vec3 min, glm::vec3 max) const {
    for (const glm::vec4& plane : planes) {
        const glm::vec3 corner(plane.x > 0.0f ? max.x : min.x,
                               plane.y > 0.0f ? max.y : min.y,
                               plane.z > 0.0f ? max.z : min.z);
        if (glm::dot(glm::vec3(plane), corner) + plane.w < 0.0f) {
            return false;
        }
    }
    return true;
}

void ChunkCuller::cull(const ChunkRenderView& data, const Frustum& frustum, glm::vec3 cameraPosition) {
    const glm::ivec3 cameraChunk = ChunkLayout::worldToChunk(cameraPosition);
    const bool layoutChanged = !initialized_ || data.layoutRevision != layoutRevision_;
    const bool reachabilityChanged = layoutChanged || data.topologyRevision != topologyRevision_ || cameraChunk != cameraChunk_;
    const bool candidatesChanged = reachabilityChanged || data.drawableRevision != drawableRevision_;

    if (layoutChanged) {
        rebuildGrid(data.chunks);
    } else if (data.topologyRevision != topologyRevision_) {
        refreshConnectivity(data.chunks);
    }
    if (reachabilityChanged) {
        rebuildReachability(data.chunks, cameraChunk);
    }
    if (candidatesChanged) {
        rebuildCandidates(data.chunks);
    }
    if (candidatesChanged || frustum != frustum_) {
        filterFrustum(data.chunks, frustum);
    }

    layoutRevision_ = data.layoutRevision;
    topologyRevision_ = data.topologyRevision;
    drawableRevision_ = data.drawableRevision;
    cameraChunk_ = cameraChunk;
    frustum_ = frustum;
    initialized_ = true;
}

bool ChunkCuller::Grid::rebase(glm::ivec3 newMin, glm::ivec3 newMax) {
    glm::ivec3 extent;
    size_t cellCount = 1;
    for (int axis = 0; axis < 3; ++axis) {
        const int64_t length = static_cast<int64_t>(newMax[axis]) - newMin[axis] + 1;
        if (newMin[axis] == std::numeric_limits<int>::min() || newMax[axis] == std::numeric_limits<int>::max() ||
            length <= 0 || length > static_cast<int64_t>(MAX_GRID_CELLS) ||
            cellCount > MAX_GRID_CELLS / static_cast<size_t>(length)) {
            return false;
        }
        extent[axis] = static_cast<int>(length);
        cellCount *= static_cast<size_t>(length);
    }

    origin = newMin;
    size = extent;
    cells.assign(cellCount, Cell{});
    return true;
}

bool ChunkCuller::Grid::contains(glm::ivec3 chunkPos) const {
    for (int axis = 0; axis < 3; ++axis) {
        const int64_t local = static_cast<int64_t>(chunkPos[axis]) - origin[axis];
        if (local < 0 || local >= size[axis]) {
            return false;
        }
    }
    return true;
}

size_t ChunkCuller::Grid::indexOf(glm::ivec3 chunkPos) const {
    assert(contains(chunkPos));
    const glm::ivec3 local = chunkPos - origin;
    return (static_cast<size_t>(local.y) * size.z + local.z) * size.x + local.x;
}

ptrdiff_t ChunkCuller::Grid::neighborOffset(glm::ivec3 faceOffset) const {
    return (static_cast<ptrdiff_t>(faceOffset.y) * size.z + faceOffset.z) * size.x + faceOffset.x;
}

void ChunkCuller::rebuildGrid(std::span<const DrawableChunk> chunks) {
    MW_PROFILE_SCOPE("ChunkCulling.Grid");

    gridValid_ = false;
    if (chunks.empty()) {
        grid_.cells.clear();
        return;
    }

    glm::ivec3 boundsMin(std::numeric_limits<int>::max());
    glm::ivec3 boundsMax(std::numeric_limits<int>::min());
    for (const DrawableChunk& chunk : chunks) {
        boundsMin = glm::min(boundsMin, chunk.chunkPos);
        boundsMax = glm::max(boundsMax, chunk.chunkPos);
    }
    if (!grid_.rebase(boundsMin - 1, boundsMax + 1)) {
        return;
    }

    assert(chunks.size() < std::numeric_limits<uint32_t>::max());
    const auto chunkCount = static_cast<uint32_t>(chunks.size());
    for (uint32_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
        Cell& cell = grid_.cells[grid_.indexOf(chunks[chunkIndex].chunkPos)];
        cell.chunkIndex = chunkIndex;
        cell.connectivity = chunks[chunkIndex].connectivity;
    }
    gridValid_ = true;
}

void ChunkCuller::refreshConnectivity(std::span<const DrawableChunk> chunks) {
    MW_PROFILE_SCOPE("ChunkCulling.Connectivity");

    if (!gridValid_) {
        return;
    }
    for (const DrawableChunk& chunk : chunks) {
        grid_.cells[grid_.indexOf(chunk.chunkPos)].connectivity = chunk.connectivity;
    }
}

void ChunkCuller::rebuildReachability(std::span<const DrawableChunk> chunks, glm::ivec3 cameraChunk) {
    MW_PROFILE_SCOPE("ChunkCulling.Reachability");

    fillQueue_.clear();
    reachable_.clear();
    const auto reachEveryChunk = [&]() {
        for (uint32_t index = 0; index < chunks.size(); ++index) {
            reachable_.push_back(index);
        }
    };
    if (!gridValid_ || !grid_.contains(cameraChunk)) {
        reachEveryChunk();
        return;
    }
    const auto seedCellIndex = static_cast<uint32_t>(grid_.indexOf(cameraChunk));
    Cell& seed = grid_.cells[seedCellIndex];
    if (seed.chunkIndex == std::numeric_limits<uint32_t>::max()) {
        reachEveryChunk();
        return;
    }

    if (++visitStamp_ == 0) {
        for (Cell& cell : grid_.cells) {
            cell.visitStamp = 0;
        }
        visitStamp_ = 1;
    }

    ptrdiff_t neighborCellOffsets[6];
    for (int face = 0; face < 6; ++face) {
        neighborCellOffsets[face] = grid_.neighborOffset(kChunkFaceOffsets[face]);
    }

    seed.visitStamp = visitStamp_;
    seed.pendingExits = kAllFaces;
    seed.propagatedExits = 0;
    fillQueue_.push_back(seedCellIndex);
    reachable_.push_back(seed.chunkIndex);

    for (size_t head = 0; head < fillQueue_.size(); ++head) {
        const uint32_t cellIndex = fillQueue_[head];
        Cell& cell = grid_.cells[cellIndex];
        uint8_t exits = cell.pendingExits;
        cell.pendingExits = 0;
        cell.propagatedExits |= exits;

        while (exits != 0) {
            const int outFace = std::countr_zero(exits);
            exits &= static_cast<uint8_t>(exits - 1);
            const auto neighborCellIndex = static_cast<uint32_t>(static_cast<ptrdiff_t>(cellIndex) + neighborCellOffsets[outFace]);
            Cell& neighbor = grid_.cells[neighborCellIndex];
            if (neighbor.chunkIndex == std::numeric_limits<uint32_t>::max()) {
                continue;
            }
            if (neighbor.visitStamp != visitStamp_) {
                neighbor.visitStamp = visitStamp_;
                neighbor.pendingExits = 0;
                neighbor.propagatedExits = 0;
                reachable_.push_back(neighbor.chunkIndex);
            }
            const int inFace = kOppositeChunkFace[outFace];
            const auto newExits = static_cast<uint8_t>(neighbor.connectivity[inFace] & ~neighbor.propagatedExits);
            if (newExits != 0) {
                if (neighbor.pendingExits == 0) {
                    fillQueue_.push_back(neighborCellIndex);
                }
                neighbor.pendingExits |= newExits;
            }
        }
    }
}

void ChunkCuller::rebuildCandidates(std::span<const DrawableChunk> chunks) {
    MW_PROFILE_SCOPE("ChunkCulling.Candidates");

    candidates_.clear();
    for (uint32_t index : reachable_) {
        if (chunks[index].binding.isValid()) {
            candidates_.push_back(index);
        }
    }
}

void ChunkCuller::filterFrustum(std::span<const DrawableChunk> chunks, const Frustum& frustum) {
    MW_PROFILE_SCOPE("ChunkCulling.Frustum");

    visible_.clear();
    const float chunkSize = static_cast<float>(ChunkLayout::SIZE);
    for (uint32_t index : candidates_) {
        const glm::vec3 chunkMin = glm::vec3(chunks[index].chunkPos) * chunkSize;
        if (frustum.testAABB(chunkMin, chunkMin + chunkSize)) {
            visible_.push_back(index);
        }
    }
}
