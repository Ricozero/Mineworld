#include "game_server.h"

#include <algorithm>
#include <cstdlib>
#include <glm/gtx/hash.hpp>
#include <optional>
#include <unordered_set>
#include <utility>

#include "chunk.h"
#include "chunk_generator.h"
#include "config.h"
#include "entity.h"
#include "helper.h"
#include "log.h"
#include "net_kcp.h"
#include "profiler.h"
#include "server_system.h"

namespace {

constexpr int kMaxChunkUpdatesPerTick = 4;
constexpr int kMaxChunkGenerationsPerTick = 8;
constexpr double kChunkGenerationTimeBudgetMs = 25.0;
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

void GameServer::registerSystem(std::unique_ptr<common_system::BaseSystem<ServerWorld>> system) {
    systems_.push_back(std::move(system));
}

void GameServer::update(float deltaTime) {
    MW_PROFILE_SCOPE("Server.Update");

    pumpNetwork();
    for (auto& system : systems_) {
        system->update(world_, deltaTime);
    }
    updateChunks();

    const float entitySnapshotInterval = 1.0f / static_cast<float>(AppConfig::instance().ticksPerSecond);
    for (auto& [sessionId, session] : sessions_) {
        if (!session.helloReceived) {
            continue;
        }
        session.entitySnapshotTimer += deltaTime;
        if (session.entitySnapshotTimer < entitySnapshotInterval) {
            continue;
        }
        session.entitySnapshotTimer -= entitySnapshotInterval;

        const NetEntitySnapshot snapshot = buildEntitySnapshot(session);
        std::vector<uint8_t> payload = serializeEntitySnapshot(snapshot, session.entitySnapshotBuilder);
        MW_PROFILE_COUNTER("Server.EntitySnapshotsOut", 1);
        MW_PROFILE_COUNTER("Server.BytesOut", static_cast<int64_t>(payload.size()));
        netServer_->sendTo(sessionId, payload);
        sendPendingChunkUpdates(session);
    }
}

entt::entity GameServer::createLocalPlayer(const std::string& name, uint32_t sessionId, glm::vec3 position, PlayerMode mode) {
    return world_.createLocalPlayer(name, sessionId, position, mode);
}

entt::entity GameServer::createRobot(const std::string& name, glm::vec3 position) {
    return world_.createRobot(name, position);
}

void GameServer::setBlock(glm::ivec3 worldPos, BlockData blockData) {
    world_.setBlock(worldPos, blockData);
    const glm::ivec3 chunkPos = Chunk::worldToChunk(worldPos);
    for (auto& [sessionId, session] : sessions_) {
        if (session.helloReceived && session.cachedVisibleChunks.count(chunkPos) > 0) {
            queueChunkUpdate(session, buildUpsertChunkUpdate(chunkPos));
        }
    }
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

void GameServer::updateSessionChunkDemand(Session& session, glm::ivec3 currentChunkPos, ChunkManager::DemandMap& demands) {
    session.lastChunkPos = currentChunkPos;
    std::unordered_set<glm::ivec3> nextVisibleChunks;
    std::unordered_set<glm::ivec3> nextRetentionChunks;
    const int horizontalRadius = AppConfig::instance().chunkViewRadiusHorizontal;
    const int verticalRadius = AppConfig::instance().chunkViewRadiusVertical;

    forEachChunkInCylinder(currentChunkPos, horizontalRadius, verticalRadius, [&](glm::ivec3 chunkPos) {
        if (world_.isChunkInBounds(chunkPos)) {
            nextVisibleChunks.insert(chunkPos);
        }
    });
    forEachChunkInRing(currentChunkPos, horizontalRadius, verticalRadius, [&](glm::ivec3 chunkPos) {
        if (world_.isChunkInBounds(chunkPos)) {
            nextRetentionChunks.insert(chunkPos);
        }
    });
    if (!session.ready) {
        nextVisibleChunks.insert(session.coreChunks.begin(), session.coreChunks.end());
    }

    for (const glm::ivec3& chunkPos : nextVisibleChunks) {
        if (session.cachedVisibleChunks.count(chunkPos) == 0) {
            session.newlyVisibleChunks.insert(chunkPos);
        }
    }

    for (const glm::ivec3& chunkPos : session.cachedVisibleChunks) {
        if (nextVisibleChunks.count(chunkPos) == 0) {
            session.newlyVisibleChunks.erase(chunkPos);
            const uint32_t revision = world_.getVoxelWorld().isChunkLoaded(chunkPos) ? world_.getChunk(chunkPos).getRevision() : 0;
            queueChunkUpdate(session, buildUnloadChunkUpdate(chunkPos, revision));
        }
    }

    for (const glm::ivec3& chunkPos : nextVisibleChunks) {
        if (session.cachedVisibleChunks.count(chunkPos) == 0 && world_.getVoxelWorld().isChunkLoaded(chunkPos)) {
            queueChunkUpdate(session, buildUpsertChunkUpdate(chunkPos));
        }

        const glm::ivec3 offset = chunkPos - currentChunkPos;
        ChunkPriorityClass priorityClass = ChunkPriorityClass::PlayerVisible;
        if (!session.ready && session.coreChunks.count(chunkPos) > 0) {
            priorityClass = ChunkPriorityClass::LoadingCore;
        } else if (session.newlyVisibleChunks.count(chunkPos) > 0) {
            priorityClass = ChunkPriorityClass::PlayerNew;
        }
        demands[chunkPos].addRequester(ChunkPriority{
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

    auto& registry = world_.getActorWorld().registry();
    for (const glm::ivec3& chunkPos : visibleChunks) {
        for (entt::entity entity : world_.getActorWorld().getEntitiesInChunk(chunkPos)) {
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

void GameServer::sendPendingChunkUpdates(Session& session) {
    std::vector<NetChunkUpdate*> candidates;
    candidates.reserve(session.pendingChunkUpdates.size());
    for (auto& [chunkPos, update] : session.pendingChunkUpdates) {
        if (update.operation == NetChunkOperation::Upsert && (session.cachedVisibleChunks.count(chunkPos) == 0 || !world_.getVoxelWorld().isChunkLoaded(chunkPos))) {
            continue;
        }
        candidates.push_back(&update);
    }

    std::sort(candidates.begin(), candidates.end(), [&](const NetChunkUpdate* a, const NetChunkUpdate* b) {
        if (a->operation != b->operation) {
            return a->operation == NetChunkOperation::Upsert;
        }
        return ivec3DistanceSq(a->chunkPos, session.lastChunkPos) < ivec3DistanceSq(b->chunkPos, session.lastChunkPos);
    });

    std::vector<glm::ivec3> sent;
    const size_t count = std::min(candidates.size(), static_cast<size_t>(kMaxChunkUpdatesPerTick));
    sent.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        NetChunkUpdate& update = *candidates[i];
        std::vector<uint8_t> payload = serializeChunkUpdate(update, session.chunkUpdateBuilder);
        MW_PROFILE_COUNTER("Server.ChunkUpdatesOut", 1);
        MW_PROFILE_COUNTER("Server.BytesOut", static_cast<int64_t>(payload.size()));
        netServer_->sendTo(session.sessionId, payload);
        sent.push_back(update.chunkPos);
    }
    for (const glm::ivec3& chunkPos : sent) {
        session.pendingChunkUpdates.erase(chunkPos);
    }
    MW_PROFILE_GAUGE("Server.PendingChunkUpdates", static_cast<double>(session.pendingChunkUpdates.size()));
}

void GameServer::queueChunkUpdate(Session& session, NetChunkUpdate update) {
    const glm::ivec3 chunkPos = update.chunkPos;
    if (update.operation == NetChunkOperation::Upsert) {
        session.newlyVisibleChunks.erase(chunkPos);
    }
    session.pendingChunkUpdates[chunkPos] = std::move(update);
}

void GameServer::updateChunks() {
    MW_PROFILE_SCOPE("Server.UpdateChunks");

    ChunkManager::DemandMap demands;
    auto& registry = world_.getActorWorld().registry();

    auto playerView = registry.view<SessionComponent, TransformComponent>();
    for (auto entity : playerView) {
        const uint32_t sessionId = playerView.get<SessionComponent>(entity).sessionId;
        auto sessionIt = sessions_.find(sessionId);
        if (sessionIt == sessions_.end() || !sessionIt->second.helloReceived) {
            continue;
        }
        updateSessionChunkDemand(sessionIt->second, world_.getActorWorld().getEntityChunk(entity), demands);
    }

    auto robotView = registry.view<RobotComponent, TransformComponent>();
    for (auto entity : robotView) {
        const glm::ivec3 entityChunk = world_.getActorWorld().getEntityChunk(entity);
        forEachChunkInBox(entityChunk, kRobotChunkViewRadius, [&](glm::ivec3 chunkPos) {
            if (!world_.isChunkInBounds(chunkPos)) {
                return;
            }
            const glm::ivec3 offset = chunkPos - entityChunk;
            demands[chunkPos].addRequester(ChunkPriority{
                ChunkPriorityClass::Robot,
                offset.x * offset.x + offset.z * offset.z,
                std::abs(offset.y),
            });
        });
    }

    const ChunkManager::TimePoint now = ChunkManager::Clock::now();
    chunkManager_.updateDemands(demands, now);
    processQueuedChunks();
    processPendingUnloads(now);

    const size_t loadedChunkCount = chunkManager_.stateCount(ChunkState::Loaded) + chunkManager_.stateCount(ChunkState::UnloadPending);
    MW_PROFILE_GAUGE("Server.LoadedChunks", static_cast<double>(loadedChunkCount));
    MW_PROFILE_GAUGE("Server.QueuedChunks", static_cast<double>(chunkManager_.stateCount(ChunkState::Queued)));
    MW_PROFILE_GAUGE("Server.RequestedChunks", static_cast<double>(chunkManager_.requestedChunkCount()));
    MW_PROFILE_GAUGE("Server.PendingUnloadChunks", static_cast<double>(chunkManager_.stateCount(ChunkState::UnloadPending)));
}

void GameServer::processQueuedChunks() {
    const std::vector<glm::ivec3> queuedChunks = chunkManager_.queuedChunks();

    const auto timeBudget = std::chrono::duration<double, std::milli>(kChunkGenerationTimeBudgetMs);
    const ChunkManager::TimePoint startTime = ChunkManager::Clock::now();
    int generatedCount = 0;

    for (const glm::ivec3& chunkPos : queuedChunks) {
        if (generatedCount >= kMaxChunkGenerationsPerTick || ChunkManager::Clock::now() - startTime >= timeBudget) {
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
        if (!commitChunkLoad(data, *generationId)) {
            chunkManager_.failGeneration(chunkPos, *generationId);
        }
    }
}

void GameServer::processPendingUnloads(ChunkManager::TimePoint now) {
    for (const glm::ivec3& chunkPos : chunkManager_.chunksReadyToUnload(now)) {
        if (!commitChunkUnload(chunkPos)) {
            chunkManager_.restoreLoaded(chunkPos);
        }
    }
}

bool GameServer::commitChunkLoad(const ChunkData& data, uint64_t generationId) {
    if (!world_.loadChunk(data)) {
        return false;
    }
    if (!chunkManager_.commitLoaded(data.chunkPos, generationId)) {
        world_.unloadChunk(data.chunkPos);
        return false;
    }

    for (auto& [sessionId, session] : sessions_) {
        if (session.helloReceived && session.cachedVisibleChunks.count(data.chunkPos) > 0) {
            queueChunkUpdate(session, buildUpsertChunkUpdate(data.chunkPos));
        }
    }
    return true;
}

bool GameServer::commitChunkUnload(glm::ivec3 chunkPos) {
    if (!chunkManager_.markUnloaded(chunkPos)) {
        return false;
    }
    return !world_.getVoxelWorld().isChunkLoaded(chunkPos) || world_.unloadChunk(chunkPos);
}

NetChunkUpdate GameServer::buildUpsertChunkUpdate(glm::ivec3 chunkPos) {
    const ChunkData data = world_.getVoxelWorld().buildChunkData(chunkPos);
    NetChunkUpdate update;
    update.chunkPos = data.chunkPos;
    update.revision = data.revision;
    update.operation = NetChunkOperation::Upsert;
    update.blocks.assign(data.blocks.begin(), data.blocks.end());
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
        entt::entity entity = world_.getEntityByName(sessionIt->second.actorName);
        if (entity != entt::null) {
            world_.destroyEntity(entity);
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

    auto& registry = world_.getActorWorld().registry();
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
    const glm::ivec3 spawnChunk = Chunk::worldToChunk(glm::ivec3(glm::floor(spawnPos)));
    hello.coreChunks.reserve((kCoreChunkRadius * 2 + 1) * (kCoreChunkRadius * 2 + 1) * (kCoreChunkRadius * 2 + 1));
    forEachChunkInBox(spawnChunk, kCoreChunkRadius, [&](glm::ivec3 chunkPos) {
        if (!world_.isChunkInBounds(chunkPos)) {
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

    auto& registry = world_.getActorWorld().registry();
    auto view = registry.view<SessionComponent, TransformComponent, PlayerComponent>();
    for (auto entity : view) {
        const auto& session = view.get<SessionComponent>(entity);
        if (session.sessionId != sessionId) {
            continue;
        }
        world_.getActorWorld().setPlayerMode(entity, input.playerMode);
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
        world_.getActorWorld().updateEntityChunk(entity, transform.position);
        break;
    }
}
