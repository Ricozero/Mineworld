#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "chunk_mesh.h"

struct ChunkMeshSlot {
    uint8_t bucket = UINT8_MAX;
    uint32_t slotIndex = 0;
    uint32_t vertexCount = 0;

    bool isValid() const { return bucket != UINT8_MAX; }
};

struct ChunkMeshBinding {
    uint16_t vertexBuffer = UINT16_MAX;
    uint32_t vertexOffset = 0;
    uint32_t vertexCount = 0;

    bool isValid() const { return vertexBuffer != UINT16_MAX; }
};

struct DrawableChunk {
    glm::ivec3 chunkPos{0};
    ChunkFaceConnectivity connectivity = 0;
    ChunkMeshBinding binding;
};

class ChunkMeshPool {
public:
    ChunkMeshPool() = default;
    ~ChunkMeshPool();

    ChunkMeshPool(const ChunkMeshPool&) = delete;
    ChunkMeshPool& operator=(const ChunkMeshPool&) = delete;

    bool initialize();
    void shutdown();
    bool isInitialized() const { return initialized_; }

    void onFrameSubmitted(uint32_t bgfxFrameNumber);

    bool upload(const std::vector<ChunkVertex>& vertices, ChunkMeshSlot& outSlot);
    void release(const ChunkMeshSlot& slot);

    ChunkMeshBinding binding(const ChunkMeshSlot& slot) const;

    uint16_t quadIndexBuffer() const { return quadIndexBuffer_; }
    static uint32_t indexCountForVertices(uint32_t vertexCount) { return vertexCount / 4 * 6; }

    size_t reservedBytes() const { return reservedBytes_; }
    size_t committedBytes() const { return committedBytes_; }
    size_t usedBytes() const { return usedBytes_; }

private:
    struct Buffer {
        uint16_t handle = UINT16_MAX;
        uint32_t liveSlots = 0;
        uint32_t idleSinceFrame = 0;
    };

    struct Bucket {
        uint32_t slotVertices = 0;
        uint32_t slotsPerBuffer = 0;
        std::vector<Buffer> buffers;
        std::vector<uint32_t> freeSlots;
    };

    struct PendingRelease {
        ChunkMeshSlot slot;
        uint32_t frame = 0;
    };

    void reclaimPending();
    void destroyIdleBuffers();
    bool growBucket(Bucket& bucket);

    bool initialized_ = false;
    uint16_t quadIndexBuffer_ = UINT16_MAX;
    std::vector<Bucket> buckets_;
    std::vector<PendingRelease> pending_;
    uint32_t frameNumber_ = 0;
    bool sawFirstFrame_ = false;
    size_t reservedBytes_ = 0;
    size_t committedBytes_ = 0;
    size_t usedBytes_ = 0;
};
