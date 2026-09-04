#include "chunk_culler.h"

#include <algorithm>
#include <cassert>
#include <climits>

#include "chunk_mesh.h"

namespace {

constexpr uint8_t kAllFaceMarks = 0x3Fu;
constexpr uint8_t kEmittedMark = 0x40u;

}  // namespace

Frustum Frustum::fromViewProjection(const float* viewProjection) {
    glm::mat4 matrix;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            matrix[column][row] = viewProjection[column * 4 + row];
        }
    }

    // Gribb/Hartmann extraction
    const glm::mat4 rows = glm::transpose(matrix);
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

void ChunkCuller::cull(const std::vector<DrawableChunk>& chunks, const Frustum& frustum, glm::vec3 cameraPosition) {
    visible_.clear();
    if (chunks.empty()) {
        return;
    }

    const float chunkWorldSize = static_cast<float>(ChunkLayout::SIZE);
    const auto emit = [&](const DrawableChunk& chunk) {
        if (!chunk.binding.isValid()) {
            return;
        }
        const glm::vec3 chunkMin = glm::vec3(chunk.chunkPos) * chunkWorldSize;
        if (frustum.testAABB(chunkMin, chunkMin + chunkWorldSize)) {
            visible_.push_back(chunk);
        }
    };
    const auto emitEveryChunk = [&]() {
        for (const DrawableChunk& chunk : chunks) {
            emit(chunk);
        }
    };

    glm::ivec3 boundsMin(INT_MAX);
    glm::ivec3 boundsMax(INT_MIN);
    for (const DrawableChunk& chunk : chunks) {
        boundsMin = glm::min(boundsMin, chunk.chunkPos);
        boundsMax = glm::max(boundsMax, chunk.chunkPos);
    }

    if (!grid_.rebase(boundsMin - 1, boundsMax + 1)) {
        emitEveryChunk();
        return;
    }

    if (++stamp_ == 0) {
        std::fill(grid_.cells.begin(), grid_.cells.end(), Cell{});
        stamp_ = 1;
    }

    const auto chunkCount = static_cast<uint32_t>(chunks.size());
    for (uint32_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
        Cell& cell = grid_.cells[grid_.indexOf(chunks[chunkIndex].chunkPos)];
        cell.stamp = stamp_;
        cell.chunkIndex = chunkIndex;
        cell.marks = 0;
    }

    const glm::ivec3 cameraChunk = ChunkLayout::worldToChunk(glm::ivec3(glm::floor(cameraPosition)));
    if (!grid_.contains(cameraChunk)) {
        emitEveryChunk();
        return;
    }
    const auto seedCellIndex = static_cast<uint32_t>(grid_.indexOf(cameraChunk));
    if (grid_.cells[seedCellIndex].stamp != stamp_) {
        emitEveryChunk();
        return;
    }

    ptrdiff_t neighborCellOffsets[6];
    for (int face = 0; face < 6; ++face) {
        neighborCellOffsets[face] = grid_.neighborOffset(kChunkFaceOffsets[face]);
    }

    fillQueue_.clear();
    grid_.cells[seedCellIndex].marks = kAllFaceMarks;
    fillQueue_.push_back(FillNode{seedCellIndex, -1});

    for (size_t head = 0; head < fillQueue_.size(); ++head) {
        const FillNode node = fillQueue_[head];
        Cell& cell = grid_.cells[node.cellIndex];
        const DrawableChunk& chunk = chunks[cell.chunkIndex];

        if ((cell.marks & kEmittedMark) == 0) {
            cell.marks |= kEmittedMark;
            emit(chunk);
        }

        for (int outFace = 0; outFace < 6; ++outFace) {
            if (node.inFace >= 0 && !chunkFacesConnected(chunk.connectivity, node.inFace, outFace)) {
                continue;
            }
            const auto neighborCellIndex = static_cast<uint32_t>(static_cast<ptrdiff_t>(node.cellIndex) + neighborCellOffsets[outFace]);
            Cell& neighbor = grid_.cells[neighborCellIndex];
            if (neighbor.stamp != stamp_) {
                continue;
            }
            const int inFace = kOppositeChunkFace[outFace];
            const auto inFaceMark = static_cast<uint8_t>(1u << inFace);
            if ((neighbor.marks & inFaceMark) != 0) {
                continue;
            }
            neighbor.marks |= inFaceMark;
            fillQueue_.push_back(FillNode{neighborCellIndex, static_cast<int8_t>(inFace)});
        }
    }
}

bool ChunkCuller::Grid::rebase(glm::ivec3 newMin, glm::ivec3 newMax) {
    constexpr int maxAxis = static_cast<int>(MAX_GRID_CELLS);
    const glm::ivec3 extent = newMax - newMin + 1;
    if (extent.x > maxAxis || extent.y > maxAxis || extent.z > maxAxis) {
        return false;
    }
    const size_t cellCount = static_cast<size_t>(extent.x) * extent.y * extent.z;
    if (cellCount > MAX_GRID_CELLS) {
        return false;
    }

    origin = newMin;
    size = extent;
    if (cellCount > cells.size()) {
        cells.assign(cellCount, Cell{});
    }
    return true;
}

bool ChunkCuller::Grid::contains(glm::ivec3 chunkPos) const {
    const glm::ivec3 local = chunkPos - origin;
    return local.x >= 0 && local.x < size.x &&
           local.y >= 0 && local.y < size.y &&
           local.z >= 0 && local.z < size.z;
}

size_t ChunkCuller::Grid::indexOf(glm::ivec3 chunkPos) const {
    assert(contains(chunkPos));
    const glm::ivec3 local = chunkPos - origin;
    return (static_cast<size_t>(local.y) * size.z + local.z) * size.x + local.x;
}

ptrdiff_t ChunkCuller::Grid::neighborOffset(glm::ivec3 faceOffset) const {
    return (static_cast<ptrdiff_t>(faceOffset.y) * size.z + faceOffset.z) * size.x + faceOffset.x;
}
