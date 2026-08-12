#include "chunk_manager.h"

#include <algorithm>
#include <tuple>

bool ChunkPriority::isHigherThan(const ChunkPriority& other) const {
    return std::tie(priorityClass, horizontalDistanceSquared, verticalDistance) <
           std::tie(other.priorityClass, other.horizontalDistanceSquared, other.verticalDistance);
}

void ChunkDemand::addRequester(ChunkPriority requesterPriority) {
    ++requesterCount;
    if (!priority || requesterPriority.isHigherThan(*priority)) {
        priority = requesterPriority;
    }
}

void ChunkDemand::addRetention() {
    ++retentionCount;
}

ChunkManager::ChunkManager(Clock::duration unloadDelay) : unloadDelay_(unloadDelay) {}

void ChunkManager::updateDemands(const DemandMap& demands, TimePoint now) {
    for (const auto& [chunkPos, _] : demands) {
        entries_.try_emplace(chunkPos);
    }

    for (auto& [chunkPos, entry] : entries_) {
        const auto demandIt = demands.find(chunkPos);
        if (demandIt != demands.end()) {
            entry.requesterCount = demandIt->second.requesterCount;
            entry.retentionCount = demandIt->second.retentionCount;
            if (demandIt->second.priority) {
                entry.priority = *demandIt->second.priority;
            }
        } else {
            entry.requesterCount = 0;
            entry.retentionCount = 0;
        }

        switch (entry.state) {
            case ChunkState::Unloaded:
                if (entry.requesterCount > 0) {
                    entry.state = ChunkState::Queued;
                }
                break;
            case ChunkState::Queued:
                if (entry.requesterCount == 0) {
                    entry.state = ChunkState::Unloaded;
                }
                break;
            case ChunkState::Generating:
                break;
            case ChunkState::ReadyToCommit:
                if (entry.requesterCount == 0) {
                    entry.state = ChunkState::Unloaded;
                }
                break;
            case ChunkState::Loaded:
                if (entry.requesterCount == 0 && entry.retentionCount == 0) {
                    entry.state = ChunkState::UnloadPending;
                    entry.unloadTime = now + unloadDelay_;
                }
                break;
            case ChunkState::UnloadPending:
                if (entry.requesterCount > 0 || entry.retentionCount > 0) {
                    entry.state = ChunkState::Loaded;
                    entry.unloadTime = TimePoint::max();
                }
                break;
        }
    }

    std::erase_if(entries_, [](const auto& item) {
        const Entry& entry = item.second;
        return entry.state == ChunkState::Unloaded && entry.requesterCount == 0 && entry.retentionCount == 0;
    });
}

std::vector<glm::ivec3> ChunkManager::queuedChunks() const {
    std::vector<glm::ivec3> chunks;
    for (const auto& [chunkPos, entry] : entries_) {
        if (entry.state == ChunkState::Queued && entry.requesterCount > 0) {
            chunks.push_back(chunkPos);
        }
    }

    std::sort(chunks.begin(), chunks.end(), [&](glm::ivec3 a, glm::ivec3 b) {
        const Entry& entryA = entries_.at(a);
        const Entry& entryB = entries_.at(b);
        if (entryA.priority.isHigherThan(entryB.priority)) {
            return true;
        }
        if (entryB.priority.isHigherThan(entryA.priority)) {
            return false;
        }
        return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
    });
    return chunks;
}

std::optional<uint64_t> ChunkManager::beginGeneration(glm::ivec3 chunkPos) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || it->second.state != ChunkState::Queued || it->second.requesterCount == 0) {
        return std::nullopt;
    }

    Entry& entry = it->second;
    entry.state = ChunkState::Generating;
    entry.generationId = nextGenerationId_++;
    return entry.generationId;
}

bool ChunkManager::finishGeneration(glm::ivec3 chunkPos, uint64_t generationId) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || it->second.state != ChunkState::Generating || it->second.generationId != generationId) {
        return false;
    }

    Entry& entry = it->second;
    if (entry.requesterCount == 0) {
        entry.state = ChunkState::Unloaded;
        return false;
    }
    entry.state = ChunkState::ReadyToCommit;
    return true;
}

bool ChunkManager::commitLoaded(glm::ivec3 chunkPos, uint64_t generationId) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || it->second.state != ChunkState::ReadyToCommit ||
        it->second.generationId != generationId || it->second.requesterCount == 0) {
        return false;
    }

    Entry& entry = it->second;
    entry.state = ChunkState::Loaded;
    entry.unloadTime = TimePoint::max();
    return true;
}

void ChunkManager::failGeneration(glm::ivec3 chunkPos, uint64_t generationId) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || it->second.generationId != generationId ||
        (it->second.state != ChunkState::Generating && it->second.state != ChunkState::ReadyToCommit)) {
        return;
    }
    it->second.state = it->second.requesterCount > 0 ? ChunkState::Queued : ChunkState::Unloaded;
}

std::vector<glm::ivec3> ChunkManager::chunksReadyToUnload(TimePoint now) const {
    std::vector<glm::ivec3> chunks;
    for (const auto& [chunkPos, entry] : entries_) {
        if (entry.state == ChunkState::UnloadPending && entry.requesterCount == 0 && entry.retentionCount == 0 &&
            now >= entry.unloadTime) {
            chunks.push_back(chunkPos);
        }
    }
    return chunks;
}

bool ChunkManager::markUnloaded(glm::ivec3 chunkPos) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || it->second.state != ChunkState::UnloadPending ||
        it->second.requesterCount != 0 || it->second.retentionCount != 0) {
        return false;
    }
    it->second.state = ChunkState::Unloaded;
    it->second.unloadTime = TimePoint::max();
    return true;
}

void ChunkManager::restoreLoaded(glm::ivec3 chunkPos) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end()) {
        return;
    }
    it->second.state = ChunkState::Loaded;
    it->second.unloadTime = TimePoint::max();
}

size_t ChunkManager::stateCount(ChunkState state) const {
    return static_cast<size_t>(std::count_if(entries_.begin(), entries_.end(), [&](const auto& item) {
        return item.second.state == state;
    }));
}

size_t ChunkManager::requestedChunkCount() const {
    return static_cast<size_t>(std::count_if(entries_.begin(), entries_.end(), [](const auto& item) {
        return item.second.requesterCount > 0;
    }));
}
