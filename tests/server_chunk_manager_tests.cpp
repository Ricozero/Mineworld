#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <climits>
#include <map>
#include <set>
#include <vector>

#include "chunk_test_support.h"
#include "server_chunk_manager.h"

namespace {

using namespace test_support;

using ChunkKey = std::array<int, 3>;

ChunkKey keyOf(glm::ivec3 chunkPos) {
    return ChunkKey{chunkPos.x, chunkPos.y, chunkPos.z};
}
using SCM = ServerChunkManager;

constexpr int kDemandWorld = 5;
constexpr int kDemandRadius = 1;
constexpr long kUnloadDelayTicks = 3;

glm::ivec3 toVec(ChunkKey key) {
    return glm::ivec3(key[0], key[1], key[2]);
}

bool inDemandWorld(glm::ivec3 pos) {
    return pos.x >= 0 && pos.x < kDemandWorld && pos.y >= 0 && pos.y < kDemandWorld &&
           pos.z >= 0 && pos.z < kDemandWorld;
}

struct DemandRequester {
    SCM::PriorityClass priorityClass = SCM::PriorityClass::Player;
    glm::ivec3 focus{0, 0, 0};
    std::vector<ChunkKey> visible;
    std::vector<ChunkKey> retention;

    void rebuild() {
        visible.clear();
        retention.clear();
        for (int dx = -kDemandRadius - 1; dx <= kDemandRadius + 1; ++dx) {
            for (int dy = -kDemandRadius - 1; dy <= kDemandRadius + 1; ++dy) {
                for (int dz = -kDemandRadius - 1; dz <= kDemandRadius + 1; ++dz) {
                    const glm::ivec3 pos = focus + glm::ivec3(dx, dy, dz);
                    if (!inDemandWorld(pos)) {
                        continue;
                    }
                    const int ring = std::max(std::abs(dx), std::max(std::abs(dy), std::abs(dz)));
                    (ring <= kDemandRadius ? visible : retention).push_back(keyOf(pos));
                }
            }
        }
        std::sort(visible.begin(), visible.end());
        std::sort(retention.begin(), retention.end());
    }
};

constexpr size_t kClassCount = static_cast<size_t>(SCM::PriorityClass::Count);

struct DemandCounts {
    std::array<uint32_t, kClassCount> perClass{};
    uint32_t requesterCount = 0;
    uint32_t retentionCount = 0;
};

struct RefManager {
    struct Entry {
        SCM::State state = SCM::State::Unloaded;
        std::array<uint32_t, kClassCount> perClass{};
        uint32_t requesterCount = 0;
        uint32_t retentionCount = 0;
        uint64_t generationId = 0;
        long unloadTick = LONG_MAX;

        SCM::PriorityClass priorityClass() const {
            for (size_t index = 0; index < kClassCount; ++index) {
                if (perClass[index] > 0) {
                    return static_cast<SCM::PriorityClass>(index);
                }
            }
            return static_cast<SCM::PriorityClass>(kClassCount - 1);
        }
    };

    std::map<ChunkKey, Entry> entries;
    uint64_t nextGenerationId = 1;

    void updateDemands(const std::map<ChunkKey, DemandCounts>& demands, const std::set<ChunkKey>& regained, long tick) {
        for (const auto& [key, counts] : demands) {
            entries.try_emplace(key);
        }
        for (auto& [key, entry] : entries) {
            const auto it = demands.find(key);
            entry.perClass = it == demands.end() ? std::array<uint32_t, kClassCount>{} : it->second.perClass;
            entry.requesterCount = it == demands.end() ? 0 : it->second.requesterCount;
            entry.retentionCount = it == demands.end() ? 0 : it->second.retentionCount;

            switch (entry.state) {
                case SCM::State::Unloaded:
                    if (entry.requesterCount > 0) {
                        entry.state = SCM::State::Queued;
                    }
                    break;
                case SCM::State::Queued:
                    if (entry.requesterCount == 0) {
                        entry.state = SCM::State::Unloaded;
                    }
                    break;
                case SCM::State::Generating:
                    break;
                case SCM::State::ReadyToCommit:
                    if (entry.requesterCount == 0) {
                        entry.state = SCM::State::Unloaded;
                    }
                    break;
                case SCM::State::Loaded:
                    if (entry.requesterCount == 0 && entry.retentionCount == 0) {
                        entry.state = SCM::State::UnloadPending;
                        entry.unloadTick = tick + kUnloadDelayTicks;
                    }
                    break;
                case SCM::State::UnloadPending:
                    if (entry.requesterCount > 0 || entry.retentionCount > 0) {
                        entry.state = SCM::State::Loaded;
                        entry.unloadTick = LONG_MAX;
                    } else if (regained.count(key) != 0) {
                        entry.unloadTick = tick + kUnloadDelayTicks;
                    }
                    break;
                default:
                    break;
            }
        }
        for (auto it = entries.begin(); it != entries.end();) {
            const Entry& entry = it->second;
            if (entry.state == SCM::State::Unloaded && entry.requesterCount == 0 && entry.retentionCount == 0) {
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::vector<ChunkKey> inState(SCM::State state) const {
        std::vector<ChunkKey> keys;
        for (const auto& [key, entry] : entries) {
            if (entry.state == state) {
                keys.push_back(key);
            }
        }
        return keys;
    }

    size_t countInState(SCM::State state) const { return inState(state).size(); }

    std::vector<glm::ivec3> topQueued(size_t limit, const std::vector<glm::ivec3>& foci) const {
        std::vector<std::pair<std::array<long long, 4>, glm::ivec3>> ordered;
        for (const auto& [key, entry] : entries) {
            if (entry.state != SCM::State::Queued) {
                continue;
            }
            const glm::ivec3 pos = toVec(key);
            long long best = (1LL << 30) - 1;
            for (const glm::ivec3& focus : foci) {
                const glm::ivec3 offset = pos - focus;
                const long long horizontal = offset.x * offset.x + offset.z * offset.z;
                best = std::min(best, horizontal * 1024 + std::abs(offset.y));
            }
            const long long rank = static_cast<long long>(entry.priorityClass()) * (1LL << 30) + best;
            ordered.push_back({{rank, pos.x, pos.y, pos.z}, pos});
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        ordered.resize(std::min(limit, ordered.size()));

        std::vector<glm::ivec3> positions;
        positions.reserve(ordered.size());
        for (const auto& [rank, pos] : ordered) {
            positions.push_back(pos);
        }
        return positions;
    }

    size_t requestedCount() const {
        size_t count = 0;
        for (const auto& [key, entry] : entries) {
            count += entry.requesterCount > 0 ? 1 : 0;
        }
        return count;
    }

    std::vector<ChunkKey> readyToUnload(long tick) const {
        std::vector<ChunkKey> keys;
        for (const auto& [key, entry] : entries) {
            if (entry.state == SCM::State::UnloadPending && entry.requesterCount == 0 &&
                entry.retentionCount == 0 && tick >= entry.unloadTick) {
                keys.push_back(key);
            }
        }
        return keys;
    }

    std::optional<uint64_t> beginGeneration(ChunkKey key) {
        auto it = entries.find(key);
        if (it == entries.end() || it->second.state != SCM::State::Queued || it->second.requesterCount == 0) {
            return std::nullopt;
        }
        it->second.state = SCM::State::Generating;
        it->second.generationId = nextGenerationId++;
        return it->second.generationId;
    }

    bool finishGeneration(ChunkKey key, uint64_t generationId) {
        auto it = entries.find(key);
        if (it == entries.end() || it->second.state != SCM::State::Generating ||
            it->second.generationId != generationId) {
            return false;
        }
        if (it->second.requesterCount == 0) {
            it->second.state = SCM::State::Unloaded;
            return false;
        }
        it->second.state = SCM::State::ReadyToCommit;
        return true;
    }

    bool commitLoaded(ChunkKey key, uint64_t generationId) {
        auto it = entries.find(key);
        if (it == entries.end() || it->second.state != SCM::State::ReadyToCommit ||
            it->second.generationId != generationId || it->second.requesterCount == 0) {
            return false;
        }
        it->second.state = SCM::State::Loaded;
        it->second.unloadTick = LONG_MAX;
        return true;
    }

    void failGeneration(ChunkKey key, uint64_t generationId) {
        auto it = entries.find(key);
        if (it == entries.end() || it->second.generationId != generationId ||
            (it->second.state != SCM::State::Generating && it->second.state != SCM::State::ReadyToCommit)) {
            return;
        }
        it->second.state = it->second.requesterCount > 0 ? SCM::State::Queued : SCM::State::Unloaded;
    }

    bool markUnloaded(ChunkKey key) {
        auto it = entries.find(key);
        if (it == entries.end() || it->second.state != SCM::State::UnloadPending ||
            it->second.requesterCount != 0 || it->second.retentionCount != 0) {
            return false;
        }
        it->second.state = SCM::State::Unloaded;
        it->second.unloadTick = LONG_MAX;
        return true;
    }

    void restoreLoaded(ChunkKey key, long tick) {
        Entry& entry = entries[key];
        if (entry.requesterCount > 0 || entry.retentionCount > 0) {
            entry.state = SCM::State::Loaded;
            entry.unloadTick = LONG_MAX;
        } else {
            entry.state = SCM::State::UnloadPending;
            entry.unloadTick = tick + kUnloadDelayTicks;
        }
    }
};

std::vector<ChunkKey> sortedKeys(const std::vector<glm::ivec3>& positions) {
    std::vector<ChunkKey> keys;
    keys.reserve(positions.size());
    for (const glm::ivec3& pos : positions) {
        keys.push_back(keyOf(pos));
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

TEST(ServerChunkManagerTest, RandomizedStateMachineMatchesReference) {
    SCOPED_TRACE("Seed: 0xC0FFEE1234567");
    Rng rng{0xC0FFEE1234567ull};
    const auto tickTime = [](long tick) {
        return SCM::TimePoint{} + std::chrono::milliseconds(tick * 100);
    };
    SCM manager(std::chrono::milliseconds(kUnloadDelayTicks * 100));
    RefManager ref;

    std::map<int, DemandRequester> requesters;
    int nextRequesterId = 1;
    const SCM::PriorityClass classes[] = {SCM::PriorityClass::LoadingCore, SCM::PriorityClass::Player, SCM::PriorityClass::Robot};

    std::set<ChunkKey> regained;
    const auto applyAdd = [&](const DemandRequester& requester) {
        for (const ChunkKey& key : requester.visible) {
            manager.addRequester(toVec(key), requester.priorityClass);
            regained.insert(key);
        }
        for (const ChunkKey& key : requester.retention) {
            manager.addRetention(toVec(key));
            regained.insert(key);
        }
    };
    const auto applyRemove = [&](const DemandRequester& requester, long tick) {
        for (const ChunkKey& key : requester.visible) {
            manager.removeRequester(toVec(key), requester.priorityClass, tickTime(tick));
        }
        for (const ChunkKey& key : requester.retention) {
            manager.removeRetention(toVec(key), tickTime(tick));
        }
    };

    for (long tick = 0; tick < 12000; ++tick) {
        SCOPED_TRACE(testing::Message() << "Tick: " << tick);
        regained.clear();

        if (!requesters.empty() && rng.below(100) < 6) {
            const auto victim = std::next(requesters.begin(), rng.below(static_cast<uint32_t>(requesters.size())));
            applyRemove(victim->second, tick);
            requesters.erase(victim);
        }
        if (requesters.size() < 4 && rng.below(100) < 20) {
            DemandRequester requester;
            requester.priorityClass = classes[rng.below(3)];
            requester.focus = glm::ivec3(rng.below(kDemandWorld), rng.below(kDemandWorld), rng.below(kDemandWorld));
            requester.rebuild();
            applyAdd(requester);
            requesters.emplace(nextRequesterId++, std::move(requester));
        }
        if (!requesters.empty() && rng.below(100) < 45) {
            auto mover = std::next(requesters.begin(), rng.below(static_cast<uint32_t>(requesters.size())));
            DemandRequester moved = mover->second;
            moved.focus = glm::ivec3(rng.below(kDemandWorld), rng.below(kDemandWorld), rng.below(kDemandWorld));
            moved.rebuild();
            if (moved.focus != mover->second.focus) {
                applyAdd(moved);
                applyRemove(mover->second, tick);
                mover->second = std::move(moved);
            }
        }

        std::map<ChunkKey, DemandCounts> demands;
        std::vector<glm::ivec3> foci;
        for (const auto& [id, requester] : requesters) {
            foci.push_back(requester.focus);
            for (const ChunkKey& key : requester.visible) {
                DemandCounts& counts = demands[key];
                ++counts.perClass[static_cast<size_t>(requester.priorityClass)];
                ++counts.requesterCount;
            }
            for (const ChunkKey& key : requester.retention) {
                ++demands[key].retentionCount;
            }
        }
        ref.updateDemands(demands, regained, tick);

        ASSERT_EQ(manager.requestedChunkCount(), ref.requestedCount()) << "requested chunk count diverged";
        for (const SCM::State state : {SCM::State::Queued, SCM::State::Generating, SCM::State::ReadyToCommit,
                                       SCM::State::Loaded, SCM::State::UnloadPending}) {
            ASSERT_EQ(manager.stateCount(state), ref.countInState(state)) << "State: " << static_cast<int>(state);
        }

        ASSERT_EQ(sortedKeys(manager.queuedChunks(1024, foci)), ref.inState(SCM::State::Queued)) << "queued set diverged";

        const size_t limit = 1 + rng.below(8);
        const std::vector<glm::ivec3> queued = manager.queuedChunks(limit, foci);
        ASSERT_EQ(queued, ref.topQueued(limit, foci)) << "queued order diverged";

        if (rng.below(4) != 0) {
            const size_t take = std::min<size_t>(queued.size(), 1 + rng.below(6));
            for (size_t i = 0; i < take; ++i) {
                const glm::ivec3 pos = queued[i];
                const ChunkKey key = keyOf(pos);
                const std::optional<uint64_t> generationId = manager.beginGeneration(pos);
                const std::optional<uint64_t> refGenerationId = ref.beginGeneration(key);
                ASSERT_EQ(generationId.has_value(), refGenerationId.has_value()) << "beginGeneration agrees";
                if (!generationId || !refGenerationId) {
                    continue;
                }
                if (rng.below(12) == 0) {
                    manager.failGeneration(pos, *generationId);
                    ref.failGeneration(key, *refGenerationId);
                    continue;
                }
                const bool finished = manager.finishGeneration(pos, *generationId);
                ASSERT_EQ(finished, ref.finishGeneration(key, *refGenerationId)) << "finishGeneration agrees";
                if (!finished) {
                    continue;
                }
                const bool committed = manager.commitLoaded(pos, *generationId);
                ASSERT_EQ(committed, ref.commitLoaded(key, *refGenerationId)) << "commitLoaded agrees";
                if (!committed) {
                    manager.failGeneration(pos, *generationId);
                    ref.failGeneration(key, *refGenerationId);
                }
            }
        }

        const std::vector<glm::ivec3> readyToUnload = manager.chunksReadyToUnload(tickTime(tick));
        ASSERT_EQ(sortedKeys(readyToUnload), ref.readyToUnload(tick)) << "chunksReadyToUnload diverged";
        for (const glm::ivec3& pos : readyToUnload) {
            const ChunkKey key = keyOf(pos);
            const bool marked = manager.markUnloaded(pos);
            ASSERT_EQ(marked, ref.markUnloaded(key)) << "markUnloaded agrees";
            if (marked && rng.below(10) == 0) {
                manager.restoreLoaded(pos, tickTime(tick));
                ref.restoreLoaded(key, tick);
            }
        }

        ASSERT_GE(manager.trackedChunkCount(), ref.entries.size()) << "the incremental manager dropped an entry the model still holds";
    }

    long tick = 12000;
    for (const auto& [id, requester] : requesters) {
        applyRemove(requester, tick);
    }
    requesters.clear();
    for (; tick < 12010; ++tick) {
        manager.queuedChunks(1024, {});
        for (const glm::ivec3& pos : manager.chunksReadyToUnload(tickTime(tick))) {
            EXPECT_TRUE(manager.markUnloaded(pos)) << "the final unload sweep marks every expired chunk";
        }
    }
    manager.queuedChunks(1024, {});
    manager.chunksReadyToUnload(tickTime(tick));
    EXPECT_EQ(manager.requestedChunkCount(), 0) << "no requester reference survives";
    EXPECT_EQ(manager.trackedChunkCount(), 0u) << "entries leaked after every requester left";
}

}  // namespace
