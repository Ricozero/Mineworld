#include "game_server.h"

#include <algorithm>
#include <cstdlib>
#include <glm/gtx/hash.hpp>
#include <optional>
#include <unordered_set>
#include <utility>

#include "chunk.h"
#include "chunk_generator.h"
#include "chunk_layout.h"
#include "config.h"
#include "entity.h"
#include "helper.h"
#include "log.h"
#include "net_kcp.h"
#include "profiler.h"
#include "server_system.h"

namespace {

constexpr size_t kMaxChunkUpsertsPerTick = 1024;
constexpr size_t kMaxChunkUpsertBytesPerTick = 32 * 1024;
constexpr size_t kMaxChunkGenerationsPerTick = 1024;
constexpr double kMaxChunkGenerationTimePerTick = 25.0;
constexpr int kRobotChunkViewRadius = 1;
constexpr int kCoreChunkRadius = 1;

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
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dz = -radius; dz <= radius; ++dz) {
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

GameServer::GameServer() {
    auto kcpServer = std::make_unique<KcpServer>(ioContext_, AppConfig::instance().port);
    kcpServer->setOnConnect([this](uint32_t sessionId) {
        onSessionConnect(sessionId);
    });
    kcpServer->setOnPacket([this](uint32_t sessionId, const std::vector<uint8_t>& packet) {
        return onSessionPacket(sessionId, packet);
    });
    kcpServer->setOnDisconnect([this](uint32_t sessionId) {
        onSessionDisconnect(sessionId);
    });
    netServer_ = std::move(kcpServer);

    registerSystem(std::make_unique<PhysicsSystem>());
}

GameServer::~GameServer() = default;

void GameServer::registerSystem(std::unique_ptr<System> system) {
    systems_.push_back(std::move(system));
}

void GameServer::update(float deltaTime) {
    MW_PROFILE_SCOPE("Server.Update");

    pumpNetwork();
    for (auto& system : systems_) {
        system->update(voxelWorld_, actorWorld_, deltaTime);
    }
    updateChunks();

    const float entitySnapshotInterval = 1.0f / static_cast<float>(AppConfig::instance().ticksPerSecond);
    size_t pendingChunkUpdateCount = 0;
    for (auto& [sessionId, session] : sessions_) {
        if (!session.helloReceived) {
            continue;
        }
        session.entitySnapshotTimer += deltaTime;
        if (session.entitySnapshotTimer >= entitySnapshotInterval) {
            session.entitySnapshotTimer -= entitySnapshotInterval;

            const NetEntitySnapshot snapshot = buildEntitySnapshot(session);
            std::vector<uint8_t> payload = serializeEntitySnapshot(snapshot, session.entitySnapshotBuilder);
            MW_PROFILE_COUNTER("Server.EntitySnapshotsOut", 1);
            MW_PROFILE_COUNTER("Server.BytesOut", static_cast<int64_t>(payload.size()));
            netServer_->sendTo(sessionId, payload);
        }
        sendChunkUpdates(session);
        pendingChunkUpdateCount += session.pendingChunkUpdates.size();
    }
    MW_PROFILE_GAUGE("Server.PendingChunkUpdates", static_cast<double>(pendingChunkUpdateCount));
}

entt::entity GameServer::createLocalPlayer(const std::string& name, uint32_t sessionId, glm::vec3 position, PlayerMode mode) {
    return actorWorld_.createLocalPlayer(name, sessionId, position, mode);
}

entt::entity GameServer::createRobot(const std::string& name, glm::vec3 position) {
    return actorWorld_.createRobot(name, position);
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

void GameServer::updateSessionChunkDemand(Session& session, glm::ivec3 currentChunkPos, ServerChunkManager::DemandMap& demands) {
    session.lastChunkPos = currentChunkPos;
    std::unordered_set<glm::ivec3> nextVisibleChunks;
    std::unordered_set<glm::ivec3> nextRetentionChunks;
    const int horizontalRadius = AppConfig::instance().chunkViewRadiusHorizontal;
    const int verticalRadius = AppConfig::instance().chunkViewRadiusVertical;

    forEachChunkInCylinder(currentChunkPos, horizontalRadius, verticalRadius, [&](glm::ivec3 chunkPos) {
        if (ChunkLayout::isChunkInWorld(chunkPos)) {
            nextVisibleChunks.insert(chunkPos);
        }
    });
    forEachChunkInRing(currentChunkPos, horizontalRadius, verticalRadius, [&](glm::ivec3 chunkPos) {
        if (ChunkLayout::isChunkInWorld(chunkPos)) {
            nextRetentionChunks.insert(chunkPos);
        }
    });
    if (!session.ready) {
        nextVisibleChunks.insert(session.coreChunks.begin(), session.coreChunks.end());
    }

    for (const glm::ivec3& chunkPos : session.cachedVisibleChunks) {
        if (nextVisibleChunks.count(chunkPos) == 0) {
            const Chunk* chunk = voxelWorld_.findChunk(chunkPos);
            const uint32_t revision = chunk != nullptr ? chunk->getRevision() : 0;
            queueChunkUpdate(session, buildUnloadChunkUpdate(chunkPos, revision));
        }
    }

    for (const glm::ivec3& chunkPos : nextVisibleChunks) {
        if (session.cachedVisibleChunks.count(chunkPos) == 0) {
            if (const Chunk* chunk = voxelWorld_.findChunk(chunkPos)) {
                queueChunkUpdate(session, buildUpsertChunkUpdate(*chunk));
            }
        }

        const glm::ivec3 offset = chunkPos - currentChunkPos;
        ServerChunkManager::PriorityClass priorityClass = ServerChunkManager::PriorityClass::Player;
        if (!session.ready && session.coreChunks.count(chunkPos) > 0) {
            priorityClass = ServerChunkManager::PriorityClass::LoadingCore;
        }
        demands[chunkPos].addRequester(ServerChunkManager::Priority{
            priorityClass,
            offset.x * offset.x + offset.z * offset.z,
            std::abs(offset.y),
        });
    }
    for (const glm::ivec3& chunkPos : nextRetentionChunks) {
        demands[chunkPos].addRetention();
    }

    session.cachedVisibleChunks = std::move(nextVisibleChunks);
}

NetEntitySnapshot GameServer::buildEntitySnapshot(Session& session) {
    MW_PROFILE_SCOPE("Server.BuildEntitySnapshot");

    NetEntitySnapshot snapshot;
    snapshot.sequence = ++session.entitySnapshotSequence;

    const auto& visibleChunks = session.cachedVisibleChunks;

    auto& registry = actorWorld_.registry();
    for (const glm::ivec3& chunkPos : visibleChunks) {
        for (entt::entity entity : actorWorld_.getEntitiesInChunk(chunkPos)) {
            if (!registry.all_of<NameComponent, TransformComponent>(entity)) {
                continue;
            }

            const auto& name = registry.get<NameComponent>(entity);
            const auto& transform = registry.get<TransformComponent>(entity);
            glm::vec3 velocity{0.0f};
            if (registry.all_of<PhysicsComponent>(entity)) {
                velocity = registry.get<PhysicsComponent>(entity).velocity;
            }
            EntityType entityType;
            if (registry.all_of<PlayerComponent>(entity)) entityType = EntityType::Player;
            else if (registry.all_of<RobotComponent>(entity)) entityType = EntityType::Robot;
            else continue;
            const PlayerMode playerMode = entityType == EntityType::Player ? registry.get<PlayerComponent>(entity).mode : PlayerMode::Survival;
            snapshot.actors.push_back(NetActorState{
                name.name,
                transform.position,
                velocity,
                transform.rotation.y,
                transform.rotation.x,
                entityType,
                playerMode,
            });
        }
    }

    return snapshot;
}

void GameServer::sendChunkUpdates(Session& session) {
    MW_PROFILE_SCOPE("Server.SendChunkUpdates");

    std::vector<NetChunkUpdate*> candidates;
    candidates.reserve(session.pendingChunkUpdates.size());
    for (auto& [chunkPos, update] : session.pendingChunkUpdates) {
        if (update.operation == NetChunkOperation::Upsert && (session.cachedVisibleChunks.count(chunkPos) == 0 || voxelWorld_.findChunk(chunkPos) == nullptr)) {
            continue;
        }
        candidates.push_back(&update);
    }

    const auto priority = [&](const NetChunkUpdate& update) {
        if (session.coreChunks.count(update.chunkPos) > 0) {
            return 0;
        }
        if (update.operation == NetChunkOperation::Unload) {
            return 1;
        }
        return 2;
    };

    std::sort(candidates.begin(), candidates.end(), [&](const NetChunkUpdate* a, const NetChunkUpdate* b) {
        const int aPriority = priority(*a);
        const int bPriority = priority(*b);
        if (aPriority != bPriority) {
            return aPriority < bPriority;
        }
        const int aDistance = ivec3DistanceSq(a->chunkPos, session.lastChunkPos);
        const int bDistance = ivec3DistanceSq(b->chunkPos, session.lastChunkPos);
        return aDistance < bDistance;
    });

    std::vector<glm::ivec3> sentChunks;
    sentChunks.reserve(candidates.size());
    size_t upsertCount = 0;
    size_t upsertBytes = 0;
    for (NetChunkUpdate* candidate : candidates) {
        NetChunkUpdate& update = *candidate;
        const bool isUpsert = update.operation == NetChunkOperation::Upsert;
        if (isUpsert && upsertCount >= kMaxChunkUpsertsPerTick) {
            continue;
        }

        std::vector<uint8_t> payload = serializeChunkUpdate(update, session.chunkUpdateBuilder);
        if (isUpsert && upsertBytes + payload.size() > kMaxChunkUpsertBytesPerTick) {
            continue;
        }

        MW_PROFILE_COUNTER("Server.ChunkUpdatesOut", 1);
        MW_PROFILE_COUNTER("Server.ChunkUpdateBytesOut", static_cast<int64_t>(payload.size()));
        MW_PROFILE_COUNTER("Server.BytesOut", static_cast<int64_t>(payload.size()));
        netServer_->sendTo(session.sessionId, payload);
        if (isUpsert) {
            ++upsertCount;
            upsertBytes += payload.size();
        }
        sentChunks.push_back(update.chunkPos);
    }
    for (const glm::ivec3& chunkPos : sentChunks) {
        session.pendingChunkUpdates.erase(chunkPos);
    }
}

void GameServer::queueChunkUpdate(Session& session, NetChunkUpdate update) {
    session.pendingChunkUpdates[update.chunkPos] = std::move(update);
}

void GameServer::updateChunks() {
    MW_PROFILE_SCOPE("Server.UpdateChunks");

    ServerChunkManager::DemandMap demands;
    auto& registry = actorWorld_.registry();

    auto playerView = registry.view<SessionComponent, TransformComponent>();
    for (auto entity : playerView) {
        const uint32_t sessionId = playerView.get<SessionComponent>(entity).sessionId;
        auto sessionIt = sessions_.find(sessionId);
        if (sessionIt == sessions_.end() || !sessionIt->second.helloReceived) {
            continue;
        }
        updateSessionChunkDemand(sessionIt->second, actorWorld_.getEntityChunk(entity), demands);
    }

    auto robotView = registry.view<RobotComponent, TransformComponent>();
    for (auto entity : robotView) {
        const glm::ivec3 entityChunk = actorWorld_.getEntityChunk(entity);
        forEachChunkInBox(entityChunk, kRobotChunkViewRadius, [&](glm::ivec3 chunkPos) {
            if (!ChunkLayout::isChunkInWorld(chunkPos)) {
                return;
            }
            const glm::ivec3 offset = chunkPos - entityChunk;
            demands[chunkPos].addRequester(ServerChunkManager::Priority{
                ServerChunkManager::PriorityClass::Robot,
                offset.x * offset.x + offset.z * offset.z,
                std::abs(offset.y),
            });
        });
    }

    const ServerChunkManager::TimePoint now = ServerChunkManager::Clock::now();
    chunkManager_.updateDemands(demands, now);
    processQueuedChunks();
    processPendingUnloads(now);

    const size_t loadedChunkCount = chunkManager_.stateCount(ServerChunkManager::State::Loaded) + chunkManager_.stateCount(ServerChunkManager::State::UnloadPending);
    MW_PROFILE_GAUGE("Server.LoadedChunks", static_cast<double>(loadedChunkCount));
    MW_PROFILE_GAUGE("Server.QueuedChunks", static_cast<double>(chunkManager_.stateCount(ServerChunkManager::State::Queued)));
    MW_PROFILE_GAUGE("Server.RequestedChunks", static_cast<double>(chunkManager_.requestedChunkCount()));
    MW_PROFILE_GAUGE("Server.PendingUnloadChunks", static_cast<double>(chunkManager_.stateCount(ServerChunkManager::State::UnloadPending)));
}

void GameServer::processQueuedChunks() {
    const std::vector<glm::ivec3> queuedChunks = chunkManager_.queuedChunks();

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
            chunkManager_.restoreLoaded(chunkPos);
        }
    }
}

bool GameServer::commitChunkLoad(glm::ivec3 chunkPos, ChunkData&& data, uint64_t generationId) {
    if (!ChunkLayout::isChunkInWorld(chunkPos) || voxelWorld_.findChunk(chunkPos) != nullptr ||
        !voxelWorld_.loadChunk(chunkPos, ChunkGenerator::INITIAL_REVISION, std::move(data))) {
        return false;
    }
    if (!actorWorld_.loadEntitiesInChunk(chunkPos)) {
        voxelWorld_.unloadChunk(chunkPos);
        return false;
    }
    if (!chunkManager_.commitLoaded(chunkPos, generationId)) {
        actorWorld_.unloadEntitiesInChunk(chunkPos);
        voxelWorld_.unloadChunk(chunkPos);
        return false;
    }
    if (const Chunk* chunk = voxelWorld_.findChunk(chunkPos)) {
        for (auto& [sessionId, session] : sessions_) {
            if (session.helloReceived && session.cachedVisibleChunks.count(chunkPos) > 0) {
                queueChunkUpdate(session, buildUpsertChunkUpdate(*chunk));
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
    return actorWorld_.unloadEntitiesInChunk(chunkPos) && voxelWorld_.unloadChunk(chunkPos);
}

NetChunkUpdate GameServer::buildUpsertChunkUpdate(const Chunk& chunk) {
    NetChunkUpdate update;
    update.chunkPos = chunk.getPosition();
    update.revision = chunk.getRevision();
    update.operation = NetChunkOperation::Upsert;
    update.blocks = chunk.getData();
    return update;
}

NetChunkUpdate GameServer::buildUnloadChunkUpdate(glm::ivec3 chunkPos, uint32_t revision) {
    NetChunkUpdate update;
    update.chunkPos = chunkPos;
    update.revision = revision;
    update.operation = NetChunkOperation::Unload;
    return update;
}

void GameServer::pumpNetwork() {
    MW_PROFILE_SCOPE("Server.PumpNetwork");

    if (!netServer_) {
        return;
    }

    netServer_->pump();
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

    if (!sessionIt->second.actorName.empty()) {
        entt::entity entity = actorWorld_.getEntityByName(sessionIt->second.actorName);
        if (entity != entt::null) {
            actorWorld_.destroyEntity(entity);
        }
    }

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

    std::string actorName = "Player" + std::to_string(nextPlayerIndex_++);
    session.actorName = actorName;

    glm::vec3 spawnPos = AppConfig::instance().spawnPosition;
    float spawnYaw = AppConfig::instance().spawnYaw;
    float spawnPitch = AppConfig::instance().spawnPitch;

    const entt::entity entity = createLocalPlayer(actorName, sessionId, spawnPos, PlayerMode::Survival);

    auto& registry = actorWorld_.registry();
    if (!registry.valid(entity) || !registry.all_of<TransformComponent>(entity)) {
        logging::error("Failed to create player for session {}", sessionId);
        return false;
    }
    auto& transform = registry.get<TransformComponent>(entity);
    transform.rotation.y = spawnYaw;
    transform.rotation.x = spawnPitch;

    NetServerHello hello;
    hello.sessionId = sessionId;
    hello.actorName = actorName;
    hello.position = spawnPos;
    hello.yaw = spawnYaw;
    hello.pitch = spawnPitch;
    hello.playerMode = PlayerMode::Survival;
    const glm::ivec3 spawnChunk = ChunkLayout::worldToChunk(glm::ivec3(glm::floor(spawnPos)));
    hello.coreChunks.reserve((kCoreChunkRadius * 2 + 1) * (kCoreChunkRadius * 2 + 1) * (kCoreChunkRadius * 2 + 1));
    forEachChunkInBox(spawnChunk, kCoreChunkRadius, [&](glm::ivec3 chunkPos) {
        if (!ChunkLayout::isChunkInWorld(chunkPos)) {
            return;
        }
        hello.coreChunks.push_back(chunkPos);
        session.coreChunks.insert(chunkPos);
    });
    netServer_->sendTo(sessionId, serializeServerHello(hello));

    logging::info("Client hello from session {}, assigned actor '{}'", sessionId, actorName);
    return true;
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
    sessionIt->second.coreChunks.clear();
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
    auto view = registry.view<SessionComponent, TransformComponent, PlayerComponent>();
    for (auto entity : view) {
        const auto& session = view.get<SessionComponent>(entity);
        if (session.sessionId != sessionId) {
            continue;
        }
        actorWorld_.setPlayerMode(entity, input.playerMode);
        auto& transform = view.get<TransformComponent>(entity);
        transform.position = input.position;
        transform.rotation.x = input.pitch;
        transform.rotation.y = input.yaw;
        if (registry.all_of<PhysicsComponent>(entity)) {
            registry.get<PhysicsComponent>(entity).velocity = input.velocity;
        }
        if (sessionIt != sessions_.end()) {
            sessionIt->second.lastProcessedInputSequence = input.sequence;
        }
        actorWorld_.updateEntityChunk(entity, transform.position);
        break;
    }
}
