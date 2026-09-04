#include "chunk_mesh_pool.h"

#include <bgfx/bgfx.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>

#include "log.h"

namespace {

constexpr uint32_t kBucketVertices[] = {256, 1024, 2048, 4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152};
static_assert(std::is_sorted(std::begin(kBucketVertices), std::end(kBucketVertices)));
static_assert(kBucketVertices[std::size(kBucketVertices) - 1] == kMaxChunkMeshVertices);

constexpr uint32_t kBufferByteBudget = 4u << 20;
constexpr size_t kMaxReservedBytes = 512u << 20;
constexpr uint32_t kReleaseFrameLatency = 3;
constexpr uint32_t kIdleBufferFrames = 600;

const bgfx::VertexLayout& chunkVertexLayout() {
    static const bgfx::VertexLayout layout = [] {
        bgfx::VertexLayout built;
        built.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Uint8, true)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .end();
        return built;
    }();
    return layout;
}

}  // namespace

ChunkMeshPool::~ChunkMeshPool() {
    shutdown();
}

bool ChunkMeshPool::initialize() {
    if (initialized_) {
        return true;
    }

    const bgfx::VertexLayout& layout = chunkVertexLayout();
    if (layout.getStride() != sizeof(ChunkVertex) || layout.getOffset(bgfx::Attrib::Position) != offsetof(ChunkVertex, x) || layout.getOffset(bgfx::Attrib::Color0) != offsetof(ChunkVertex, abgr)) {
        logging::error("Chunk vertex layout does not match ChunkVertex");
        shutdown();
        return false;
    }

    std::vector<uint16_t> quadIndices;
    quadIndices.reserve(indexCountForVertices(kMaxChunkMeshVertices));
    for (uint32_t vertex = 0; vertex + 4 <= kMaxChunkMeshVertices; vertex += 4) {
        quadIndices.push_back(static_cast<uint16_t>(vertex + 0));
        quadIndices.push_back(static_cast<uint16_t>(vertex + 1));
        quadIndices.push_back(static_cast<uint16_t>(vertex + 2));
        quadIndices.push_back(static_cast<uint16_t>(vertex + 0));
        quadIndices.push_back(static_cast<uint16_t>(vertex + 2));
        quadIndices.push_back(static_cast<uint16_t>(vertex + 3));
    }
    const bgfx::IndexBufferHandle indexBuffer = bgfx::createIndexBuffer(bgfx::copy(quadIndices.data(), static_cast<uint32_t>(quadIndices.size() * sizeof(uint16_t))));
    if (!bgfx::isValid(indexBuffer)) {
        logging::error("Failed to create chunk quad index buffer");
        shutdown();
        return false;
    }
    quadIndexBuffer_ = indexBuffer.idx;

    buckets_.reserve(std::size(kBucketVertices));
    for (uint32_t slotVertices : kBucketVertices) {
        Bucket bucket;
        bucket.slotVertices = slotVertices;
        bucket.slotsPerBuffer = std::max(1u, kBufferByteBudget / (slotVertices * static_cast<uint32_t>(sizeof(ChunkVertex))));
        buckets_.push_back(std::move(bucket));
    }

    initialized_ = true;
    return true;
}

void ChunkMeshPool::shutdown() {
    for (Bucket& bucket : buckets_) {
        for (Buffer& buffer : bucket.buffers) {
            bgfx::DynamicVertexBufferHandle handle{buffer.handle};
            if (bgfx::isValid(handle)) {
                bgfx::destroy(handle);
            }
            buffer.handle = UINT16_MAX;
        }
    }
    buckets_.clear();
    pending_.clear();

    bgfx::IndexBufferHandle indexBuffer{quadIndexBuffer_};
    if (bgfx::isValid(indexBuffer)) {
        bgfx::destroy(indexBuffer);
    }
    quadIndexBuffer_ = UINT16_MAX;

    reservedBytes_ = 0;
    committedBytes_ = 0;
    usedBytes_ = 0;
    frameNumber_ = 0;
    sawFirstFrame_ = false;
    initialized_ = false;
}

void ChunkMeshPool::onFrameSubmitted(uint32_t bgfxFrameNumber) {
    if (!sawFirstFrame_) {
        sawFirstFrame_ = true;
        for (PendingRelease& pending : pending_) {
            pending.frame = bgfxFrameNumber;
        }
        for (Bucket& bucket : buckets_) {
            for (Buffer& buffer : bucket.buffers) {
                buffer.idleSinceFrame = bgfxFrameNumber;
            }
        }
    }

    frameNumber_ = bgfxFrameNumber;
    reclaimPending();
    destroyIdleBuffers();
}

bool ChunkMeshPool::growBucket(Bucket& bucket) {
    const uint32_t bufferVertices = bucket.slotVertices * bucket.slotsPerBuffer;
    const size_t bufferBytes = static_cast<size_t>(bufferVertices) * sizeof(ChunkVertex);
    if (reservedBytes_ + bufferBytes > kMaxReservedBytes) {
        return false;
    }

    const bgfx::DynamicVertexBufferHandle handle = bgfx::createDynamicVertexBuffer(bufferVertices, chunkVertexLayout());
    if (!bgfx::isValid(handle)) {
        return false;
    }

    auto vacant = std::find_if(bucket.buffers.begin(), bucket.buffers.end(), [](const Buffer& buffer) {
        return buffer.handle == UINT16_MAX;
    });
    if (vacant == bucket.buffers.end()) {
        bucket.buffers.push_back(Buffer{});
        vacant = std::prev(bucket.buffers.end());
    }
    vacant->handle = handle.idx;
    vacant->liveSlots = 0;
    vacant->idleSinceFrame = frameNumber_;

    const auto bufferIndex = static_cast<uint32_t>(vacant - bucket.buffers.begin());
    bucket.freeSlots.reserve(bucket.freeSlots.size() + bucket.slotsPerBuffer);
    for (uint32_t slot = bucket.slotsPerBuffer; slot > 0; --slot) {
        bucket.freeSlots.push_back(bufferIndex * bucket.slotsPerBuffer + (slot - 1));
    }
    reservedBytes_ += bufferBytes;
    return true;
}

bool ChunkMeshPool::upload(const std::vector<ChunkVertex>& vertices, ChunkMeshSlot& outSlot) {
    outSlot = ChunkMeshSlot{};
    if (!initialized_) {
        return false;
    }
    if (vertices.empty()) {
        return true;
    }

    const auto vertexCount = static_cast<uint32_t>(vertices.size());
    const auto bucketIt = std::find_if(buckets_.begin(), buckets_.end(), [vertexCount](const Bucket& candidate) {
        return candidate.slotVertices >= vertexCount;
    });
    if (bucketIt == buckets_.end()) {
        return false;
    }

    Bucket& bucket = *bucketIt;
    if (bucket.freeSlots.empty() && !growBucket(bucket)) {
        return false;
    }

    const uint32_t slotIndex = bucket.freeSlots.back();
    bucket.freeSlots.pop_back();

    Buffer& buffer = bucket.buffers[slotIndex / bucket.slotsPerBuffer];
    assert(buffer.handle != UINT16_MAX);
    ++buffer.liveSlots;

    outSlot.bucket = static_cast<uint8_t>(bucketIt - buckets_.begin());
    outSlot.slotIndex = slotIndex;
    outSlot.vertexCount = vertexCount;

    bgfx::update(bgfx::DynamicVertexBufferHandle{buffer.handle},
                 (slotIndex % bucket.slotsPerBuffer) * bucket.slotVertices,
                 bgfx::copy(vertices.data(), vertexCount * static_cast<uint32_t>(sizeof(ChunkVertex))));

    committedBytes_ += static_cast<size_t>(bucket.slotVertices) * sizeof(ChunkVertex);
    usedBytes_ += static_cast<size_t>(vertexCount) * sizeof(ChunkVertex);
    return true;
}

void ChunkMeshPool::release(const ChunkMeshSlot& slot) {
    if (!slot.isValid()) {
        return;
    }
    assert(slot.bucket < buckets_.size());

    const size_t vertexBytes = static_cast<size_t>(slot.vertexCount) * sizeof(ChunkVertex);
    assert(usedBytes_ >= vertexBytes);
    usedBytes_ -= vertexBytes;
    pending_.push_back(PendingRelease{slot, frameNumber_});
}

ChunkMeshBinding ChunkMeshPool::binding(const ChunkMeshSlot& slot) const {
    ChunkMeshBinding result;
    if (!slot.isValid()) {
        return result;
    }
    assert(slot.bucket < buckets_.size());
    const Bucket& bucket = buckets_[slot.bucket];
    const Buffer& buffer = bucket.buffers[slot.slotIndex / bucket.slotsPerBuffer];
    assert(buffer.handle != UINT16_MAX);
    result.vertexBuffer = buffer.handle;
    result.vertexOffset = (slot.slotIndex % bucket.slotsPerBuffer) * bucket.slotVertices;
    result.vertexCount = slot.vertexCount;
    return result;
}

void ChunkMeshPool::reclaimPending() {
    size_t reclaimed = 0;
    while (reclaimed < pending_.size() && frameNumber_ - pending_[reclaimed].frame >= kReleaseFrameLatency) {
        const ChunkMeshSlot& slot = pending_[reclaimed].slot;
        ++reclaimed;
        assert(slot.bucket < buckets_.size());
        Bucket& bucket = buckets_[slot.bucket];
        Buffer& buffer = bucket.buffers[slot.slotIndex / bucket.slotsPerBuffer];
        bucket.freeSlots.push_back(slot.slotIndex);
        assert(buffer.liveSlots > 0);
        if (--buffer.liveSlots == 0) {
            buffer.idleSinceFrame = frameNumber_;
        }
        const size_t slotBytes = static_cast<size_t>(bucket.slotVertices) * sizeof(ChunkVertex);
        assert(committedBytes_ >= slotBytes);
        committedBytes_ -= slotBytes;
    }
    if (reclaimed > 0) {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<ptrdiff_t>(reclaimed));
    }
}

void ChunkMeshPool::destroyIdleBuffers() {
    for (Bucket& bucket : buckets_) {
        for (size_t bufferIndex = 0; bufferIndex < bucket.buffers.size(); ++bufferIndex) {
            Buffer& buffer = bucket.buffers[bufferIndex];
            if (buffer.handle == UINT16_MAX || buffer.liveSlots != 0 || frameNumber_ - buffer.idleSinceFrame < kIdleBufferFrames) {
                continue;
            }

            bgfx::destroy(bgfx::DynamicVertexBufferHandle{buffer.handle});
            buffer.handle = UINT16_MAX;

            const uint32_t firstSlot = static_cast<uint32_t>(bufferIndex) * bucket.slotsPerBuffer;
            const uint32_t lastSlot = firstSlot + bucket.slotsPerBuffer;
            std::erase_if(bucket.freeSlots, [firstSlot, lastSlot](uint32_t slot) {
                return slot >= firstSlot && slot < lastSlot;
            });

            const size_t bufferBytes = static_cast<size_t>(bucket.slotVertices) * bucket.slotsPerBuffer * sizeof(ChunkVertex);
            assert(reservedBytes_ >= bufferBytes);
            reservedBytes_ -= bufferBytes;
        }
    }
}
