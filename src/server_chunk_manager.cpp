#include "server_chunk_manager.h"

#include <algorithm>
#include <cstdlib>
#include <tuple>

namespace {

constexpr int kMaxHorizontalDistanceSquared = (1 << 20) - 1;
constexpr int kMaxVerticalDistance = (1 << 10) - 1;

static_assert(static_cast<uint32_t>(ServerChunkManager::PriorityClass::Count) <= 4, "PriorityClass must fit in 2 bits");

uint32_t sortKey(ServerChunkManager::PriorityClass priorityClass, glm::ivec3 chunkPos, const std::vector<glm::ivec3>& foci) {
    uint32_t distance = (1u << 30) - 1;
    for (const glm::ivec3& focus : foci) {
        const glm::ivec3 offset = chunkPos - focus;
        const auto horizontal = static_cast<uint32_t>(std::min(offset.x * offset.x + offset.z * offset.z, kMaxHorizontalDistanceSquared));
        const auto vertical = static_cast<uint32_t>(std::min(std::abs(offset.y), kMaxVerticalDistance));
        distance = std::min(distance, (horizontal << 10) | vertical);
    }
    return (static_cast<uint32_t>(priorityClass) << 30) | distance;
}

}  // namespace

ServerChunkManager::PriorityClass ServerChunkManager::Entry::priorityClass() const {
    for (size_t index = 0; index < requesterCounts.size(); ++index) {
        if (requesterCounts[index] > 0) {
            return static_cast<PriorityClass>(index);
        }
    }
    return static_cast<PriorityClass>(requesterCounts.size() - 1);
}

ServerChunkManager::ServerChunkManager(Clock::duration unloadDelay) : unloadDelay_(unloadDelay) {}

void ServerChunkManager::addRequester(glm::ivec3 chunkPos, PriorityClass priorityClass) {
    Entry& entry = getOrCreateEntry(chunkPos);
    ++entry.requesterCounts[static_cast<size_t>(priorityClass)];
    if (++entry.requesterCount == 1) {
        ++requestedCount_;
    }
    onDemandGained(chunkPos, entry);
}

void ServerChunkManager::removeRequester(glm::ivec3 chunkPos, PriorityClass priorityClass, TimePoint now) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || it->second.requesterCounts[static_cast<size_t>(priorityClass)] == 0) {
        return;
    }

    Entry& entry = it->second;
    --entry.requesterCounts[static_cast<size_t>(priorityClass)];
    if (--entry.requesterCount == 0) {
        --requestedCount_;
    }
    onDemandLost(chunkPos, entry, now);
    eraseIfUnused(it);
}

void ServerChunkManager::addRetention(glm::ivec3 chunkPos) {
    Entry& entry = getOrCreateEntry(chunkPos);
    ++entry.retentionCount;
    onDemandGained(chunkPos, entry);
}

void ServerChunkManager::removeRetention(glm::ivec3 chunkPos, TimePoint now) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || it->second.retentionCount == 0) {
        return;
    }

    Entry& entry = it->second;
    --entry.retentionCount;
    onDemandLost(chunkPos, entry, now);
    eraseIfUnused(it);
}

std::vector<glm::ivec3> ServerChunkManager::queuedChunks(size_t limit, const std::vector<glm::ivec3>& foci) {
    struct QueuedChunk {
        uint32_t key;
        glm::ivec3 pos;
    };

    std::vector<QueuedChunk> queued;
    queued.reserve(queuedList_.size());

    size_t kept = 0;
    for (const glm::ivec3 chunkPos : queuedList_) {
        auto it = entries_.find(chunkPos);
        if (it == entries_.end()) {
            continue;
        }
        if (it->second.state != State::Queued) {
            it->second.inQueuedList = false;
            eraseIfUnused(it);
            continue;
        }
        queuedList_[kept++] = chunkPos;
        queued.push_back(QueuedChunk{sortKey(it->second.priorityClass(), chunkPos, foci), chunkPos});
    }
    queuedList_.resize(kept);

    const auto less = [](const QueuedChunk& a, const QueuedChunk& b) {
        return std::tie(a.key, a.pos.x, a.pos.y, a.pos.z) < std::tie(b.key, b.pos.x, b.pos.y, b.pos.z);
    };
    const auto take = static_cast<ptrdiff_t>(std::min(limit, queued.size()));
    std::nth_element(queued.begin(), queued.begin() + take, queued.end(), less);
    queued.resize(static_cast<size_t>(take));
    std::sort(queued.begin(), queued.end(), less);

    std::vector<glm::ivec3> chunks;
    chunks.reserve(queued.size());
    for (const QueuedChunk& item : queued) {
        chunks.push_back(item.pos);
    }
    return chunks;
}

std::optional<uint64_t> ServerChunkManager::beginGeneration(glm::ivec3 chunkPos) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || it->second.state != State::Queued || it->second.requesterCount == 0) {
        return std::nullopt;
    }

    Entry& entry = it->second;
    setState(entry, State::Generating);
    entry.generationId = nextGenerationId_++;
    return entry.generationId;
}

bool ServerChunkManager::finishGeneration(glm::ivec3 chunkPos, uint64_t generationId) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || it->second.state != State::Generating || it->second.generationId != generationId) {
        return false;
    }

    Entry& entry = it->second;
    if (entry.requesterCount == 0) {
        setState(entry, State::Unloaded);
        eraseIfUnused(it);
        return false;
    }
    setState(entry, State::ReadyToCommit);
    return true;
}

bool ServerChunkManager::commitLoaded(glm::ivec3 chunkPos, uint64_t generationId) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || it->second.state != State::ReadyToCommit || it->second.generationId != generationId || it->second.requesterCount == 0) {
        return false;
    }

    Entry& entry = it->second;
    setState(entry, State::Loaded);
    entry.unloadTime = TimePoint::max();
    return true;
}

void ServerChunkManager::failGeneration(glm::ivec3 chunkPos, uint64_t generationId) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || it->second.generationId != generationId || (it->second.state != State::Generating && it->second.state != State::ReadyToCommit)) {
        return;
    }

    Entry& entry = it->second;
    if (entry.requesterCount > 0) {
        enqueue(chunkPos, entry);
        return;
    }
    setState(entry, State::Unloaded);
    eraseIfUnused(it);
}

std::vector<glm::ivec3> ServerChunkManager::chunksReadyToUnload(TimePoint now) {
    std::vector<glm::ivec3> chunks;

    size_t kept = 0;
    for (const glm::ivec3 chunkPos : unloadPendingList_) {
        auto it = entries_.find(chunkPos);
        if (it == entries_.end()) {
            continue;
        }
        if (it->second.state != State::UnloadPending) {
            it->second.inUnloadPendingList = false;
            eraseIfUnused(it);
            continue;
        }
        unloadPendingList_[kept++] = chunkPos;
        if (now >= it->second.unloadTime) {
            chunks.push_back(chunkPos);
        }
    }
    unloadPendingList_.resize(kept);

    return chunks;
}

bool ServerChunkManager::markUnloaded(glm::ivec3 chunkPos) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || it->second.state != State::UnloadPending || it->second.requesterCount != 0 || it->second.retentionCount != 0) {
        return false;
    }
    setState(it->second, State::Unloaded);
    it->second.unloadTime = TimePoint::max();
    eraseIfUnused(it);
    return true;
}

void ServerChunkManager::restoreLoaded(glm::ivec3 chunkPos, TimePoint now) {
    Entry& entry = getOrCreateEntry(chunkPos);
    if (entry.requesterCount > 0 || entry.retentionCount > 0) {
        setState(entry, State::Loaded);
        entry.unloadTime = TimePoint::max();
        return;
    }
    scheduleUnload(chunkPos, entry, now);
}

ServerChunkManager::Entry& ServerChunkManager::getOrCreateEntry(glm::ivec3 chunkPos) {
    const auto [it, inserted] = entries_.try_emplace(chunkPos);
    if (inserted) {
        ++stateCounts_[static_cast<size_t>(it->second.state)];
    }
    return it->second;
}

void ServerChunkManager::setState(Entry& entry, State newState) {
    if (entry.state == newState) {
        return;
    }
    --stateCounts_[static_cast<size_t>(entry.state)];
    ++stateCounts_[static_cast<size_t>(newState)];
    entry.state = newState;
}

void ServerChunkManager::enqueue(glm::ivec3 chunkPos, Entry& entry) {
    setState(entry, State::Queued);
    if (!entry.inQueuedList) {
        entry.inQueuedList = true;
        queuedList_.push_back(chunkPos);
    }
}

void ServerChunkManager::scheduleUnload(glm::ivec3 chunkPos, Entry& entry, TimePoint now) {
    setState(entry, State::UnloadPending);
    entry.unloadTime = now + unloadDelay_;
    if (!entry.inUnloadPendingList) {
        entry.inUnloadPendingList = true;
        unloadPendingList_.push_back(chunkPos);
    }
}

void ServerChunkManager::onDemandGained(glm::ivec3 chunkPos, Entry& entry) {
    switch (entry.state) {
        case State::Unloaded:
            if (entry.requesterCount > 0) {
                enqueue(chunkPos, entry);
            }
            break;
        case State::UnloadPending:
            setState(entry, State::Loaded);
            entry.unloadTime = TimePoint::max();
            break;
        default:
            break;
    }
}

void ServerChunkManager::onDemandLost(glm::ivec3 chunkPos, Entry& entry, TimePoint now) {
    switch (entry.state) {
        case State::Queued:
        case State::ReadyToCommit:
            if (entry.requesterCount == 0) {
                setState(entry, State::Unloaded);
            }
            break;
        case State::Loaded:
            if (entry.requesterCount == 0 && entry.retentionCount == 0) {
                scheduleUnload(chunkPos, entry, now);
            }
            break;
        default:
            break;
    }
}

void ServerChunkManager::eraseIfUnused(std::unordered_map<glm::ivec3, Entry>::iterator it) {
    const Entry& entry = it->second;
    if (entry.state != State::Unloaded || entry.requesterCount > 0 || entry.retentionCount > 0 || entry.inQueuedList || entry.inUnloadPendingList) {
        return;
    }
    --stateCounts_[static_cast<size_t>(entry.state)];
    entries_.erase(it);
}
