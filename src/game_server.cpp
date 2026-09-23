#include "game_server.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <glm/gtx/hash.hpp>
#include <iterator>
#include <optional>
#include <tuple>
#include <utility>

#include "chunk.h"
#include "chunk_generator.h"
#include "chunk_layout.h"
#include "config.h"
#include "entity.h"
#include "log.h"
#include "net_kcp.h"
#include "profiler.h"
#include "server_system.h"
#include "text.h"

namespace {

constexpr size_t kMaxChunkUpsertsPerTick = 1024;
constexpr size_t kMaxChunkUpsertBytesPerTick = 128 * 1024;
constexpr size_t kMaxChunkGenerationsPerTick = 1024;
constexpr double kMaxChunkGenerationTimePerTick = 25.0;
constexpr int kRobotChunkViewRadius = 1;
constexpr int kCoreChunkRadius = 1;
constexpr auto kChunkUnloadDelay = std::chrono::seconds(3);

bool chunkLess(glm::ivec3 a, glm::ivec3 b) {
    return std::tie(a.x, a.z, a.y) < std::tie(b.x, b.z, b.y);
}

int64_t chunkDistanceSquared(glm::ivec3 a, glm::ivec3 b) {
    const int64_t dx = static_cast<int64_t>(a.x) - b.x;
    const int64_t dy = static_cast<int64_t>(a.y) - b.y;
    const int64_t dz = static_cast<int64_t>(a.z) - b.z;
    return dx * dx + dy * dy + dz * dz;
}

template <typename Func>
void forEachChunkInCylinder(glm::ivec3 center, int horizontalRadius, int verticalRadius, Func&& func) {
    const int horizontalRadiusSq = horizontalRadius * horizontalRadius;

    for (int dx = -horizontalRadius; dx <= horizontalRadius; ++dx) {
        for (int dz = -horizontalRadius; dz <= horizontalRadius; ++dz) {
            if (dx * dx + dz * dz > horizontalRadiusSq) {
                continue;
            }
            for (int dy = -verticalRadius; dy <= verticalRadius; ++dy) {
                func(center + glm::ivec3(dx, dy, dz));
            }
        }
    }
}

template <typename Func>
void forEachChunkInBox(glm::ivec3 center, int radius, Func&& func) {
    for (int dx = -radius; dx <= radius; ++dx) {
        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dy = -radius; dy <= radius; ++dy) {
                func(center + glm::ivec3(dx, dy, dz));
            }
        }
    }
}

template <typename Func>
void forEachChunkInRing(glm::ivec3 center, int horizontalRadius, int verticalRadius, Func&& func) {
    const int innerRadiusSq = horizontalRadius * horizontalRadius;
    const int outerRadius = horizontalRadius + 1;
    const int outerRadiusSq = outerRadius * outerRadius;

    for (int dx = -outerRadius; dx <= outerRadius; ++dx) {
        for (int dz = -outerRadius; dz <= outerRadius; ++dz) {
            const int distanceSq = dx * dx + dz * dz;
            if (distanceSq <= innerRadiusSq || distanceSq > outerRadiusSq) {
                continue;
            }
            for (int dy = -verticalRadius; dy <= verticalRadius; ++dy) {
                func(center + glm::ivec3(dx, dy, dz));
            }
        }
    }
}

}  // namespace

GameServer::GameServer() : GameServer(nullptr) {}

GameServer::GameServer(std::unique_ptr<INetServer> netServer) : chunkManager_(kChunkUnloadDelay), netServer_(std::move(netServer)) {
    if (!netServer_) {
        netServer_ = std::make_unique<KcpServer>(ioContext_, AppConfig::instance().port);
    }
    registerSystem(std::make_unique<PhysicsSystem>());
}

GameServer::~GameServer() = default;

void GameServer::registerSystem(std::unique_ptr<System> system) {
    systems_.push_back(std::move(system));
}

void GameServer::update(float deltaTime) {
    MW_PROFILE_SCOPE("Server.Update");

    pumpNetwork();
    processNetworkEvents();
    for (auto& system : systems_) {
        system->update(voxelWorld_, actorWorld_, deltaTime);
    }
    updateChunks();

    actorManager_.collectSnapshot();
    size_t pendingChunkUpdateCount = 0;
    for (auto& [sessionId, session] : sessions_) {
        if (!session.helloReceived) {
            continue;
        }
        sendEntitySnapshot(session, deltaTime);
        sendChunkUpdates(session);
        pendingChunkUpdateCount += session.pendingChunkUpdates.size();
    }
    MW_PROFILE_GAUGE("Server.PendingChunkUpdates", static_cast<double>(pendingChunkUpdateCount));
}

GameServer::Session& GameServer::getOrCreateSession(uint32_t sessionId) {
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        return it->second;
    }
    Session session;
    session.sessionId = sessionId;
    auto [inserted, _] = sessions_.emplace(sessionId, std::move(session));
    return inserted->second;
}

void GameServer::rebuildSessionChunkDemand(Session& session, glm::ivec3 currentChunkPos, ServerChunkManager::TimePoint now) {
    const int horizontalRadius = AppConfig::instance().chunkViewRadiusHorizontal;
    const int verticalRadius = AppConfig::instance().chunkViewRadiusVertical;

    std::vector<glm::ivec3> nextVisibleChunks;
    nextVisibleChunks.reserve(session.cachedVisibleChunks.size());
    forEachChunkInCylinder(currentChunkPos, horizontalRadius, verticalRadius, [&](glm::ivec3 chunkPos) {
        if (ChunkLayout::isChunkInWorld(chunkPos)) {
            nextVisibleChunks.push_back(chunkPos);
        }
    });
    assert(std::is_sorted(nextVisibleChunks.begin(), nextVisibleChunks.end(), chunkLess));

    if (!session.coreChunks.empty()) {
        assert(std::is_sorted(session.coreChunks.begin(), session.coreChunks.end(), chunkLess));
        std::vector<glm::ivec3> merged;
        merged.reserve(nextVisibleChunks.size() + session.coreChunks.size());
        std::set_union(nextVisibleChunks.begin(), nextVisibleChunks.end(), session.coreChunks.begin(), session.coreChunks.end(), std::back_inserter(merged), chunkLess);
        nextVisibleChunks = std::move(merged);
    }

    std::vector<glm::ivec3> nextRetentionChunks;
    nextRetentionChunks.reserve(session.cachedRetentionChunks.size());
    forEachChunkInRing(currentChunkPos, horizontalRadius, verticalRadius, [&](glm::ivec3 chunkPos) {
        if (ChunkLayout::isChunkInWorld(chunkPos)) {
            nextRetentionChunks.push_back(chunkPos);
        }
    });

    std::vector<glm::ivec3> diff;
    std::set_difference(nextVisibleChunks.begin(), nextVisibleChunks.end(), session.cachedVisibleChunks.begin(), session.cachedVisibleChunks.end(), std::back_inserter(diff), chunkLess);
    for (const glm::ivec3& chunkPos : diff) {
        chunkManager_.addRequester(chunkPos, ServerChunkManager::PriorityClass::Player);
        if (const Chunk* chunk = voxelWorld_.findChunk(chunkPos)) {
            queueChunkUpdate(session, *chunk, ChunkOperation::Upsert);
        }
    }

    diff.clear();
    std::set_difference(nextRetentionChunks.begin(), nextRetentionChunks.end(), session.cachedRetentionChunks.begin(), session.cachedRetentionChunks.end(), std::back_inserter(diff), chunkLess);
    for (const glm::ivec3& chunkPos : diff) {
        chunkManager_.addRetention(chunkPos);
    }

    diff.clear();
    std::set_difference(session.cachedVisibleChunks.begin(), session.cachedVisibleChunks.end(), nextVisibleChunks.begin(), nextVisibleChunks.end(), std::back_inserter(diff), chunkLess);
    for (const glm::ivec3& chunkPos : diff) {
        chunkManager_.removeRequester(chunkPos, ServerChunkManager::PriorityClass::Player, now);
        if (const Chunk* chunk = voxelWorld_.findChunk(chunkPos)) {
            queueChunkUpdate(session, *chunk, ChunkOperation::Unload);
        }
    }

    diff.clear();
    std::set_difference(session.cachedRetentionChunks.begin(), session.cachedRetentionChunks.end(), nextRetentionChunks.begin(), nextRetentionChunks.end(), std::back_inserter(diff), chunkLess);
    for (const glm::ivec3& chunkPos : diff) {
        chunkManager_.removeRetention(chunkPos, now);
    }

    session.cachedVisibleChunks = std::move(nextVisibleChunks);
    session.cachedRetentionChunks = std::move(nextRetentionChunks);
    session.lastChunkPos = currentChunkPos;
}

void GameServer::releaseSessionChunkDemand(Session& session, ServerChunkManager::TimePoint now) {
    for (const glm::ivec3& chunkPos : session.cachedVisibleChunks) {
        chunkManager_.removeRequester(chunkPos, ServerChunkManager::PriorityClass::Player, now);
    }
    session.cachedVisibleChunks.clear();
    for (const glm::ivec3& chunkPos : session.cachedRetentionChunks) {
        chunkManager_.removeRetention(chunkPos, now);
    }
    session.cachedRetentionChunks.clear();
    releaseSessionCoreChunkDemand(session, now);
}

void GameServer::releaseSessionCoreChunkDemand(Session& session, ServerChunkManager::TimePoint now) {
    if (session.coreChunks.empty()) {
        return;
    }
    for (const glm::ivec3& chunkPos : session.coreChunks) {
        chunkManager_.removeRequester(chunkPos, ServerChunkManager::PriorityClass::LoadingCore, now);
    }
    session.coreChunks.clear();
    session.lastChunkPos = Session::kInvalidChunkPos;
}

void GameServer::updateRobotChunkDemand(entt::entity entity, glm::ivec3 currentChunkPos, ServerChunkManager::TimePoint now) {
    const auto [it, inserted] = robotChunks_.try_emplace(entity, currentChunkPos);
    if (!inserted && it->second == currentChunkPos) {
        return;
    }

    forEachChunkInBox(currentChunkPos, kRobotChunkViewRadius, [&](glm::ivec3 chunkPos) {
        if (ChunkLayout::isChunkInWorld(chunkPos)) {
            chunkManager_.addRequester(chunkPos, ServerChunkManager::PriorityClass::Robot);
        }
    });
    if (!inserted) {
        releaseRobotChunkDemand(it->second, now);
    }
    it->second = currentChunkPos;
}

void GameServer::releaseRobotChunkDemand(glm::ivec3 lastChunkPos, ServerChunkManager::TimePoint now) {
    forEachChunkInBox(lastChunkPos, kRobotChunkViewRadius, [&](glm::ivec3 chunkPos) {
        if (ChunkLayout::isChunkInWorld(chunkPos)) {
            chunkManager_.removeRequester(chunkPos, ServerChunkManager::PriorityClass::Robot, now);
        }
    });
}

void GameServer::sendEntitySnapshot(Session& session, float deltaTime) {
    const float interval = 1.0f / static_cast<float>(AppConfig::instance().ticksPerSecond);
    session.entitySnapshotTimer += deltaTime;
    if (session.entitySnapshotTimer < interval) return;

    session.entitySnapshotTimer -= interval;
    ++session.entitySnapshotSequence;
    std::vector<const NetActorState*> snapshotActors;
    const auto* transform = actorWorld_.registry().try_get<TransformComponent>(session.actor);
    if (transform) {
        actorManager_.selectSnapshot(transform->position, AppConfig::instance().entityViewRadius, snapshotActors);
    }
    const auto payload = serializeEntitySnapshot(session.entitySnapshotSequence, snapshotActors, session.entitySnapshotBuilder);
    if (sendPacket(session.sessionId, payload)) {
        MW_PROFILE_COUNTER("Server.EntitySnapshotsOut", 1);
    }
}

void GameServer::sendChunkUpdates(Session& session) {
    std::vector<glm::ivec3> coreUpserts;
    std::vector<glm::ivec3> upserts;
    std::vector<NetChunkUnload> unloads;
    for (auto it = session.pendingChunkUpdates.begin(); it != session.pendingChunkUpdates.end();) {
        const auto& [pos, update] = *it;
        if (update.operation == ChunkOperation::Unload) {
            unloads.push_back(NetChunkUnload{pos, update.revision});
        } else if (!std::binary_search(session.cachedVisibleChunks.begin(), session.cachedVisibleChunks.end(), pos, chunkLess) || voxelWorld_.findChunk(pos) == nullptr) {
            it = session.pendingChunkUpdates.erase(it);
            continue;
        } else if (std::binary_search(session.coreChunks.begin(), session.coreChunks.end(), pos, chunkLess)) {
            coreUpserts.push_back(pos);
        } else {
            upserts.push_back(pos);
        }
        ++it;
    }
    const auto nearFirst = [&](glm::ivec3 a, glm::ivec3 b) {
        const auto aDistance = chunkDistanceSquared(a, session.lastChunkPos);
        const auto bDistance = chunkDistanceSquared(b, session.lastChunkPos);
        return aDistance != bDistance ? aDistance < bDistance : chunkLess(a, b);
    };
    std::sort(coreUpserts.begin(), coreUpserts.end(), nearFirst);
    std::sort(upserts.begin(), upserts.end(), nearFirst);

    size_t upsertCount = 0;
    size_t upsertBytes = 0;
    const auto recordBatch = [&](ChunkOperation operation, size_t count, const std::vector<uint8_t>& payload) {
        MW_PROFILE_COUNTER(operation == ChunkOperation::Upsert ? "Server.ChunkUpsertBatchesOut" : "Server.ChunkUnloadBatchesOut", 1);
        MW_PROFILE_COUNTER("Server.ChunkUpdatesOut", static_cast<int64_t>(count));
        MW_PROFILE_COUNTER("Server.ChunkUpdateBytesOut", static_cast<int64_t>(payload.size()));
    };
    const auto sendUpserts = [&](const std::vector<glm::ivec3>& positions) {
        constexpr size_t kEstimatedBatchOverheadBytes = 128;
        constexpr size_t kEstimatedEntryOverheadBytes = 64;
        size_t cursor = 0;
        while (cursor < positions.size() && upsertCount < kMaxChunkUpsertsPerTick && upsertBytes < kMaxChunkUpsertBytesPerTick) {
            const size_t byteLimit = std::min(kMaxChunkBatchBytes, kMaxChunkUpsertBytesPerTick - upsertBytes);
            const size_t countLimit = std::min(kMaxChunkUpsertsPerBatch, kMaxChunkUpsertsPerTick - upsertCount);
            std::vector<NetChunkUpsert> batch;
            batch.reserve(countLimit);
            size_t estimatedBytes = kEstimatedBatchOverheadBytes;
            for (size_t i = cursor; i < positions.size() && batch.size() < countLimit; ++i) {
                const Chunk* chunk = voxelWorld_.findChunk(positions[i]);
                assert(chunk != nullptr);
                auto snapshot = chunk->getEncodedSnapshot();
                const size_t entryBytes = snapshot->data.bytes.size() + kEstimatedEntryOverheadBytes;
                if (!batch.empty() && estimatedBytes + entryBytes > byteLimit) {
                    break;
                }
                estimatedBytes += entryBytes;
                batch.push_back(NetChunkUpsert{positions[i], std::move(snapshot)});
            }
            auto payload = serializeChunkUpsertBatch(batch, session.chunkUpdateBuilder);
            while (batch.size() > 1 && (payload.empty() || payload.size() > byteLimit)) {
                batch.resize(batch.size() / 2);
                payload = serializeChunkUpsertBatch(batch, session.chunkUpdateBuilder);
            }
            if (payload.size() > byteLimit || !sendPacket(session.sessionId, payload)) {
                return false;
            }
            recordBatch(ChunkOperation::Upsert, batch.size(), payload);
            upsertCount += batch.size();
            upsertBytes += payload.size();
            cursor += batch.size();
            for (const auto& entry : batch) {
                session.pendingChunkUpdates.erase(entry.chunkPos);
            }
        }
        return true;
    };

    const bool coreSent = sendUpserts(coreUpserts);
    for (size_t cursor = 0; cursor < unloads.size();) {
        const size_t count = std::min(kMaxChunkUnloadsPerBatch, unloads.size() - cursor);
        const auto batch = std::span(unloads).subspan(cursor, count);
        const auto payload = serializeChunkUnloadBatch(batch, session.chunkUpdateBuilder);
        if (!sendPacket(session.sessionId, payload)) {
            return;
        }
        recordBatch(ChunkOperation::Unload, count, payload);
        for (const auto& entry : batch) {
            session.pendingChunkUpdates.erase(entry.chunkPos);
        }
        cursor += count;
    }
    if (coreSent) {
        sendUpserts(upserts);
    }
}

void GameServer::queueChunkUpdate(Session& session, const Chunk& chunk, ChunkOperation operation) {
    session.pendingChunkUpdates[chunk.getPosition()] = PendingChunkUpdate{operation, operation == ChunkOperation::Unload ? chunk.getRevision() : 0};
}

void GameServer::updateChunks() {
    MW_PROFILE_SCOPE("Server.UpdateChunks");

    const ServerChunkManager::TimePoint now = ServerChunkManager::Clock::now();
    auto& registry = actorWorld_.registry();
    std::vector<glm::ivec3> chunkFoci;

    auto playerView = registry.view<SessionComponent, TransformComponent>();
    for (auto entity : playerView) {
        const uint32_t sessionId = playerView.get<SessionComponent>(entity).sessionId;
        auto sessionIt = sessions_.find(sessionId);
        if (sessionIt == sessions_.end() || !sessionIt->second.helloReceived) {
            continue;
        }
        const glm::ivec3 entityChunk = ChunkLayout::worldToChunk(playerView.get<TransformComponent>(entity).position);
        if (entityChunk != sessionIt->second.lastChunkPos) {
            rebuildSessionChunkDemand(sessionIt->second, entityChunk, now);
        }
        chunkFoci.push_back(entityChunk);
    }

    for (auto it = robotChunks_.begin(); it != robotChunks_.end();) {
        if (registry.valid(it->first) && registry.all_of<RobotComponent, TransformComponent>(it->first)) {
            ++it;
            continue;
        }
        releaseRobotChunkDemand(it->second, now);
        it = robotChunks_.erase(it);
    }

    auto robotView = registry.view<RobotComponent, TransformComponent>();
    for (auto entity : robotView) {
        const glm::ivec3 entityChunk = ChunkLayout::worldToChunk(robotView.get<TransformComponent>(entity).position);
        updateRobotChunkDemand(entity, entityChunk, now);
        chunkFoci.push_back(entityChunk);
    }

    processQueuedChunks(chunkFoci);
    processPendingUnloads(now);

    const size_t loadedChunkCount = chunkManager_.stateCount(ServerChunkManager::State::Loaded) + chunkManager_.stateCount(ServerChunkManager::State::UnloadPending);
    MW_PROFILE_GAUGE("Server.LoadedChunks", static_cast<double>(loadedChunkCount));
    MW_PROFILE_GAUGE("Server.QueuedChunks", static_cast<double>(chunkManager_.stateCount(ServerChunkManager::State::Queued)));
    MW_PROFILE_GAUGE("Server.RequestedChunks", static_cast<double>(chunkManager_.requestedChunkCount()));
    MW_PROFILE_GAUGE("Server.PendingUnloadChunks", static_cast<double>(chunkManager_.stateCount(ServerChunkManager::State::UnloadPending)));
    MW_PROFILE_GAUGE("Server.TrackedChunks", static_cast<double>(chunkManager_.trackedChunkCount()));
}

void GameServer::processQueuedChunks(const std::vector<glm::ivec3>& chunkFoci) {
    const std::vector<glm::ivec3> queuedChunks = chunkManager_.queuedChunks(kMaxChunkGenerationsPerTick, chunkFoci);

    const auto timeBudget = std::chrono::duration<double, std::milli>(kMaxChunkGenerationTimePerTick);
    const ServerChunkManager::TimePoint startTime = ServerChunkManager::Clock::now();
    int generatedCount = 0;

    for (const glm::ivec3& chunkPos : queuedChunks) {
        if (generatedCount >= kMaxChunkGenerationsPerTick || ServerChunkManager::Clock::now() - startTime >= timeBudget) {
            break;
        }

        const std::optional<uint64_t> generationId = chunkManager_.beginGeneration(chunkPos);
        if (!generationId) {
            continue;
        }

        ChunkData data = ChunkGenerator::generate(chunkPos);
        if (!chunkManager_.finishGeneration(chunkPos, *generationId)) {
            continue;
        }
        if (!commitChunkLoad(chunkPos, std::move(data), *generationId)) {
            chunkManager_.failGeneration(chunkPos, *generationId);
        }
    }
}

void GameServer::processPendingUnloads(ServerChunkManager::TimePoint now) {
    for (const glm::ivec3& chunkPos : chunkManager_.chunksReadyToUnload(now)) {
        if (!commitChunkUnload(chunkPos)) {
            chunkManager_.restoreLoaded(chunkPos, now);
        }
    }
}

bool GameServer::commitChunkLoad(glm::ivec3 chunkPos, ChunkData&& data, uint64_t generationId) {
    if (!ChunkLayout::isChunkInWorld(chunkPos) || voxelWorld_.findChunk(chunkPos) != nullptr ||
        !voxelWorld_.loadChunk(chunkPos, ChunkGenerator::kInitialRevision, std::move(data))) {
        return false;
    }
    if (!chunkManager_.commitLoaded(chunkPos, generationId)) {
        voxelWorld_.unloadChunk(chunkPos);
        return false;
    }
    if (const Chunk* chunk = voxelWorld_.findChunk(chunkPos)) {
        for (auto& [sessionId, session] : sessions_) {
            const auto& visibleChunks = session.cachedVisibleChunks;
            if (session.helloReceived && std::binary_search(visibleChunks.begin(), visibleChunks.end(), chunkPos, chunkLess)) {
                queueChunkUpdate(session, *chunk, ChunkOperation::Upsert);
            }
        }
    }
    return true;
}

bool GameServer::commitChunkUnload(glm::ivec3 chunkPos) {
    if (!chunkManager_.markUnloaded(chunkPos)) {
        return false;
    }
    if (voxelWorld_.findChunk(chunkPos) == nullptr) {
        return true;
    }
    return voxelWorld_.unloadChunk(chunkPos);
}

void GameServer::pumpNetwork() {
    MW_PROFILE_SCOPE("Server.PumpNetwork");

    if (!netServer_) {
        return;
    }

    netServer_->pump();
}

void GameServer::processNetworkEvents() {
    MW_PROFILE_SCOPE("Server.ProcessNetworkEvents");
    NetEvent event;
    while (netServer_->popEvent(event)) {
        switch (event.type) {
            case NetEventType::Connected:
                onSessionConnect(event.sessionId);
                break;
            case NetEventType::Packet:
                if (sessions_.contains(event.sessionId) && !onSessionPacket(event.sessionId, event.payload)) {
                    netServer_->close(event.sessionId);
                }
                break;
            case NetEventType::Disconnected:
                onSessionDisconnect(event.sessionId);
                break;
        }
    }
}

bool GameServer::sendPacket(uint32_t sessionId, std::span<const uint8_t> payload) {
    if (payload.empty() || !netServer_->send(sessionId, payload)) {
        return false;
    }
    MW_PROFILE_COUNTER("Server.PacketsOut", 1);
    MW_PROFILE_COUNTER("Server.BytesOut", static_cast<int64_t>(payload.size()));
    return true;
}

void GameServer::onSessionConnect(uint32_t sessionId) {
    logging::info("Session {} connected", sessionId);
    getOrCreateSession(sessionId);
}

void GameServer::onSessionDisconnect(uint32_t sessionId) {
    auto sessionIt = sessions_.find(sessionId);
    if (sessionIt == sessions_.end()) {
        return;
    }

    actorWorld_.destroyEntity(sessionIt->second.actor);
    releaseSessionChunkDemand(sessionIt->second, ServerChunkManager::Clock::now());

    logging::info("Session {} disconnected", sessionId);
    sessions_.erase(sessionIt);
}

bool GameServer::onSessionPacket(uint32_t sessionId, const std::vector<uint8_t>& packet) {
    MW_PROFILE_COUNTER("Server.PacketsIn", 1);
    MW_PROFILE_COUNTER("Server.BytesIn", static_cast<int64_t>(packet.size()));

    using Payload = mineworld::net::NetMessagePayload;
    switch (getPacketType(packet)) {
        case Payload::ClientHello:
            return onClientHello(sessionId);
        case Payload::ClientDisconnect:
            return false;
        case Payload::ClientReady:
            onClientReady(sessionId);
            return true;
        case Payload::ClientInput: {
            NetClientInput input;
            if (deserializeClientInput(packet, input)) {
                onClientInput(sessionId, input);
            }
            return true;
        }
        case Payload::CommandRequest: {
            CommandRequest command;
            if (deserializeCommandRequest(packet, command)) {
                return sendPacket(sessionId, serializeCommandResponse(onCommandRequest(sessionId, command)));
            }
            return true;
        }
        case Payload::ChatRequest: {
            std::string text;
            if (!deserializeChatRequest(packet, text)) {
                return sendPacket(sessionId, serializeCommandResponse({0, false, "Invalid chat message."}));
            }
            return onChatRequest(sessionId, text);
        }
        default:
            logging::warn("Ignored unknown client packet from session {}", sessionId);
            return true;
    }
}

bool GameServer::onClientHello(uint32_t sessionId) {
    auto& session = getOrCreateSession(sessionId);
    if (session.helloReceived) {
        logging::warn("Duplicate ClientHello from session {}", sessionId);
        return true;
    }
    session.helloReceived = true;

    glm::vec3 spawnPos = AppConfig::instance().spawnPosition;
    float spawnYaw = AppConfig::instance().spawnYaw;
    float spawnPitch = AppConfig::instance().spawnPitch;

    const ActorId actorId = actorManager_.allocateId();
    std::string actorName = "Player" + std::to_string(actorId);
    const entt::entity entity = actorWorld_.createLocalPlayer(actorId, actorName, sessionId, spawnPos, PlayerMode::Survival);
    session.actor = entity;

    auto& registry = actorWorld_.registry();
    if (!registry.valid(entity) || !registry.all_of<TransformComponent>(entity)) {
        logging::error("Failed to create player {} '{}' for session {}", actorId, actorName, sessionId);
        return false;
    }
    auto& transform = registry.get<TransformComponent>(entity);
    transform.rotation.y = spawnYaw;
    transform.rotation.x = spawnPitch;

    logging::info("Created player {} '{}' for session {}", actorId, actorName, sessionId);

    NetServerHello hello;
    hello.sessionId = sessionId;
    hello.actorId = actorId;
    hello.actorName = actorName;
    hello.position = spawnPos;
    hello.yaw = spawnYaw;
    hello.pitch = spawnPitch;
    hello.playerMode = PlayerMode::Survival;
    const glm::ivec3 spawnChunk = ChunkLayout::worldToChunk(spawnPos);
    const size_t coreChunkCount = (kCoreChunkRadius * 2 + 1) * (kCoreChunkRadius * 2 + 1) * (kCoreChunkRadius * 2 + 1);
    hello.coreChunks.reserve(coreChunkCount);
    session.coreChunks.reserve(coreChunkCount);
    forEachChunkInBox(spawnChunk, kCoreChunkRadius, [&](glm::ivec3 chunkPos) {
        if (!ChunkLayout::isChunkInWorld(chunkPos)) {
            return;
        }
        hello.coreChunks.push_back(chunkPos);
        session.coreChunks.push_back(chunkPos);
        chunkManager_.addRequester(chunkPos, ServerChunkManager::PriorityClass::LoadingCore);
    });
    assert(std::is_sorted(session.coreChunks.begin(), session.coreChunks.end(), chunkLess));
    return sendPacket(sessionId, serializeServerHello(hello));
}

void GameServer::onClientReady(uint32_t sessionId) {
    auto sessionIt = sessions_.find(sessionId);
    if (sessionIt == sessions_.end() || !sessionIt->second.helloReceived) {
        logging::warn("ClientReady from unknown session {}", sessionId);
        return;
    }
    if (sessionIt->second.ready) {
        logging::warn("Duplicate ClientReady from session {}", sessionId);
        return;
    }

    sessionIt->second.ready = true;
    releaseSessionCoreChunkDemand(sessionIt->second, ServerChunkManager::Clock::now());
    logging::info("Session {} is ready", sessionId);
}

void GameServer::onClientInput(uint32_t sessionId, const NetClientInput& input) {
    auto sessionIt = sessions_.find(sessionId);
    if (sessionIt == sessions_.end() || !sessionIt->second.ready) {
        return;
    }
    if (input.sequence <= sessionIt->second.lastProcessedInputSequence) {
        return;
    }

    auto& registry = actorWorld_.registry();
    const auto entity = sessionIt->second.actor;
    if (!registry.valid(entity)) {
        return;
    }
    actorWorld_.setPlayerMode(entity, input.playerMode);
    auto& transform = registry.get<TransformComponent>(entity);
    transform.position = input.position;
    transform.rotation.x = input.pitch;
    transform.rotation.y = input.yaw;
    if (auto* physics = registry.try_get<PhysicsComponent>(entity)) {
        physics->velocity = input.velocity;
    }
    sessionIt->second.lastProcessedInputSequence = input.sequence;
}

CommandResponse GameServer::executeConsoleCommand(std::string_view text) {
    auto parsed = parseCommandLine(text);
    if (!parsed.command) return {0, false, std::move(parsed.error)};
    return onCommandRequest(std::nullopt, *parsed.command);
}

bool GameServer::onChatRequest(uint32_t sessionId, const std::string& text) {
    const auto source = sessions_.find(sessionId);
    if (source == sessions_.end() || !source->second.ready) {
        return sendPacket(sessionId, serializeCommandResponse({0, false, "Session is not ready."}));
    }
    const auto& registry = actorWorld_.registry();
    const auto* name = registry.try_get<NameComponent>(source->second.actor);
    if (!name) return sendPacket(sessionId, serializeCommandResponse({0, false, "Player name is unavailable."}));
    const ChatMessage message{name->name, text};
    const auto packet = serializeChatMessage(message);
    if (packet.empty()) return sendPacket(sessionId, serializeCommandResponse({0, false, "Invalid chat message."}));
    for (const auto& [id, session] : sessions_) {
        if (!sendPacket(id, packet)) netServer_->close(id);
    }
    logging::info("{}: {}", message.name, message.text);
    return true;
}

CommandResponse GameServer::onCommandRequest(std::optional<uint32_t> sessionId, const CommandRequest& command) {
    const auto result = [&](bool success, std::string message) {
        return CommandResponse{command.requestId, success, std::move(message)};
    };
    entt::entity playerEntity = entt::null;
    if (sessionId) {
        auto sessionIt = sessions_.find(*sessionId);
        if (sessionIt == sessions_.end() || !sessionIt->second.ready) return result(false, "Session is not ready.");
        if (command.requestId == 0 || command.requestId <= sessionIt->second.lastCommandRequestId) return result(false, "Invalid command request ID.");
        sessionIt->second.lastCommandRequestId = command.requestId;
        playerEntity = sessionIt->second.actor;
    }
    if (std::string error = validateCommand(command); !error.empty()) return result(false, std::move(error));
    auto& registry = actorWorld_.registry();
    switch (command.operation) {
        case CommandOperation::Help: {
            auto response = commandHelp(command.arguments.empty() ? std::string_view{} : std::get<std::string>(command.arguments[0]));
            response.requestId = command.requestId;
            return response;
        }
        case CommandOperation::CreateRobot: {
            const std::string robotName = command.arguments.empty() ? "" : std::get<std::string>(command.arguments[0]);
            if (!isValidName(robotName)) return result(false, "Name must be single-line UTF-8, at most " + std::to_string(kMaxNameCharacters) + " characters.");
            glm::vec3 position{0};
            if (command.arguments.size() == 2) {
                position = std::get<glm::vec3>(command.arguments[1]);
            } else {
                const auto* transform = registry.try_get<TransformComponent>(playerEntity);
                if (!transform) return result(false, "Console commands require an explicit position. Use /help create_robot.");
                position = transform->position;
            }
            if (!glm::all(glm::greaterThanEqual(position, glm::vec3(ChunkLayout::kWorldMin))) ||
                !glm::all(glm::lessThan(position, glm::vec3(ChunkLayout::kWorldMax)))) {
                return result(false, "Position is outside the world.");
            }
            const auto id = actorManager_.allocateId();
            const auto name = robotName.empty() ? "Robot" + std::to_string(id) : robotName;
            if (actorWorld_.createRobot(id, name, position) == entt::null) return result(false, "Failed to create robot.");
            return result(true, "Created robot " + std::to_string(id) + " '" + name + "'.");
        }
        case CommandOperation::DestroyRobot: {
            entt::entity entity = entt::null;
            if (!command.arguments.empty()) {
                if (const auto* id = std::get_if<int64_t>(&command.arguments[0])) {
                    if (*id <= 0) return result(false, "Parameter 'id' must be positive.");
                    entity = actorWorld_.getEntity(static_cast<ActorId>(*id));
                } else {
                    const auto& name = std::get<std::string>(command.arguments[0]);
                    if (!isValidName(name)) return result(false, "Name must be single-line UTF-8, at most " + std::to_string(kMaxNameCharacters) + " characters.");
                    for (const auto id : actorWorld_.findActorsByName(name)) {
                        const auto candidate = actorWorld_.getEntity(id);
                        if (registry.all_of<RobotComponent>(candidate)) {
                            entity = candidate;
                            break;
                        }
                    }
                }
            } else {
                auto robots = registry.view<RobotComponent>();
                if (robots.begin() != robots.end()) entity = *robots.begin();
            }
            if (!registry.valid(entity)) return result(false, "Robot not found.");
            if (!registry.all_of<RobotComponent>(entity)) return result(false, "Target is not a robot.");
            const ActorId id = registry.get<ActorComponent>(entity).id;
            const auto* name = registry.try_get<NameComponent>(entity);
            const std::string actorName = name ? name->name : std::string{};
            if (!actorWorld_.destroyActor(id)) return result(false, "Failed to destroy robot.");
            return result(true, "Destroyed robot " + std::to_string(id) + " '" + actorName + "'.");
        }
        default: return result(false, "Unknown command operation.");
    }
}
