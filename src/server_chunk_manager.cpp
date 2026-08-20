#include "server_chunk_manager.h"

#include <algorithm>
#include <tuple>

bool ServerChunkManager::Priority::isHigherThan(const Priority& other) const {
    return std::tie(priorityClass, horizontalDistanceSquared, verticalDistance) <
           std::tie(other.priorityClass, other.horizontalDistanceSquared, other.verticalDistance);
}

void ServerChunkManager::Demand::addRequester(Priority requesterPriority) {
    ++requesterCount;
    if (!priority || requesterPriority.isHigherThan(*priority)) {
        priority = requesterPriority;
    }
}

void ServerChunkManager::Demand::addRetention() {
    ++retentionCount;
}

ServerChunkManager::ServerChunkManager(Clock::duration unloadDelay) : unloadDelay_(unloadDelay) {}

void ServerChunkManager::updateDemands(const DemandMap& demands, TimePoint now) {
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
            case State::Unloaded:
                if (entry.requesterCount > 0) {
                    entry.state = State::Queued;
                }
                break;
            case State::Queued:
                if (entry.requesterCount == 0) {
                    entry.state = State::Unloaded;
                }
                break;
            case State::Generating:
                break;
            case State::ReadyToCommit:
                if (entry.requesterCount == 0) {
                    entry.state = State::Unloaded;
                }
                break;
            case State::Loaded:
                if (entry.requesterCount == 0 && entry.retentionCount == 0) {
                    entry.state = State::UnloadPending;
                    entry.unloadTime = now + unloadDelay_;
                }
                break;
            case State::UnloadPending:
                if (entry.requesterCount > 0 || entry.retentionCount > 0) {
                    entry.state = State::Loaded;
                    entry.unloadTime = TimePoint::max();
                }
                break;
        }
    }

    std::erase_if(entries_, [](const auto& item) {
        const Entry& entry = item.second;
        return entry.state == State::Unloaded && entry.requesterCount == 0 && entry.retentionCount == 0;
    });
}

std::vector<glm::ivec3> ServerChunkManager::queuedChunks() const {
    std::vector<glm::ivec3> chunks;
    for (const auto& [chunkPos, entry] : entries_) {
        if (entry.state == State::Queued && entry.requesterCount > 0) {
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

std::optional<uint64_t> ServerChunkManager::beginGeneration(glm::ivec3 chunkPos) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || it->second.state != State::Queued || it->second.requesterCount == 0) {
        return std::nullopt;
    }

    Entry& entry = it->second;
    entry.state = State::Generating;
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
        entry.state = State::Unloaded;
        return false;
    }
    entry.state = State::ReadyToCommit;
    return true;
}

bool ServerChunkManager::commitLoaded(glm::ivec3 chunkPos, uint64_t generationId) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || it->second.state != State::ReadyToCommit ||
        it->second.generationId != generationId || it->second.requesterCount == 0) {
        return false;
    }

    Entry& entry = it->second;
    entry.state = State::Loaded;
    entry.unloadTime = TimePoint::max();
    return true;
}

void ServerChunkManager::failGeneration(glm::ivec3 chunkPos, uint64_t generationId) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || it->second.generationId != generationId ||
        (it->second.state != State::Generating && it->second.state != State::ReadyToCommit)) {
        return;
    }
    it->second.state = it->second.requesterCount > 0 ? State::Queued : State::Unloaded;
}

std::vector<glm::ivec3> ServerChunkManager::chunksReadyToUnload(TimePoint now) const {
    std::vector<glm::ivec3> chunks;
    for (const auto& [chunkPos, entry] : entries_) {
        if (entry.state == State::UnloadPending && entry.requesterCount == 0 && entry.retentionCount == 0 &&
            now >= entry.unloadTime) {
            chunks.push_back(chunkPos);
        }
    }
    return chunks;
}

bool ServerChunkManager::markUnloaded(glm::ivec3 chunkPos) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end() || it->second.state != State::UnloadPending ||
        it->second.requesterCount != 0 || it->second.retentionCount != 0) {
        return false;
    }
    it->second.state = State::Unloaded;
    it->second.unloadTime = TimePoint::max();
    return true;
}

void ServerChunkManager::restoreLoaded(glm::ivec3 chunkPos) {
    auto it = entries_.find(chunkPos);
    if (it == entries_.end()) {
        return;
    }
    it->second.state = State::Loaded;
    it->second.unloadTime = TimePoint::max();
}

size_t ServerChunkManager::stateCount(State state) const {
    return static_cast<size_t>(std::count_if(entries_.begin(), entries_.end(), [&](const auto& item) {
        return item.second.state == state;
    }));
}

size_t ServerChunkManager::requestedChunkCount() const {
    return static_cast<size_t>(std::count_if(entries_.begin(), entries_.end(), [](const auto& item) {
        return item.second.requesterCount > 0;
    }));
}
