#include "game_server.h"

#include <algorithm>
#include <glm/gtx/hash.hpp>
#include <unordered_set>
#include <utility>

#include "chunk.h"
#include "config.h"
#include "entity.h"
#include "helper.h"
#include "log.h"
#include "net_kcp.h"
#include "profiler.h"
#include "server_system.h"

namespace {

constexpr int kMaxChunkUpdatesPerTick = 4;
constexpr float kChunkUnloadDelaySeconds = 3.0f;
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
    updateVisibleChunks(deltaTime);

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
    entt::entity entity = world_.createLocalPlayer(name, sessionId, position, mode);
    updateVisibleChunks(0.0f);
    return entity;
}

entt::entity GameServer::createRobot(const std::string& name, glm::vec3 position) {
    entt::entity entity = world_.createRobot(name, position);
    updateVisibleChunks(0.0f);
    return entity;
}

bool GameServer::loadChunk(glm::ivec3 chunkPos) {
    if (!world_.loadChunk(chunkPos)) {
        return false;
    }

    for (auto& [sessionId, session] : sessions_) {
        if (session.helloReceived && session.cachedVisibleChunks.count(chunkPos) > 0) {
            queueChunkUpdate(session, buildUpsertChunkUpdate(chunkPos));
        }
    }
    return true;
}

bool GameServer::unloadChunk(glm::ivec3 chunkPos) {
    if (!world_.getVoxelWorld().isChunkLoaded(chunkPos)) {
        return false;
    }
    const uint32_t revision = world_.getChunk(chunkPos).getRevision();
    if (!world_.unloadChunk(chunkPos)) {
        return false;
    }

    for (auto& [sessionId, session] : sessions_) {
        if (session.helloReceived) {
            queueChunkUpdate(session, buildUnloadChunkUpdate(chunkPos, revision));
        }
    }
    return true;
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

void GameServer::updateSessionVisibleChunks(Session& session) {
    auto& registry = world_.getActorWorld().registry();
    auto view = registry.view<SessionComponent, TransformComponent>();

    glm::ivec3 currentChunkPos{INT_MAX, INT_MAX, INT_MAX};
    for (auto entity : view) {
        const auto& sessionComp = view.get<SessionComponent>(entity);
        if (sessionComp.sessionId != session.sessionId) {
            continue;
        }
        currentChunkPos = world_.getActorWorld().getEntityChunk(entity);
        break;
    }

    if (currentChunkPos.x == INT_MAX) {
        return;
    }

    if (currentChunkPos == session.lastChunkPos && !session.cachedVisibleChunks.empty()) {
        return;
    }

    session.lastChunkPos = currentChunkPos;
    std::unordered_set<glm::ivec3> nextVisibleChunks;

    forEachChunkInCylinder(currentChunkPos, AppConfig::instance().chunkViewRadiusHorizontal, AppConfig::instance().chunkViewRadiusVertical, [&](glm::ivec3 chunkPos) {
        if (world_.isChunkInBounds(chunkPos)) {
            nextVisibleChunks.insert(chunkPos);
        }
    });
    if (!session.ready) {
        nextVisibleChunks.insert(session.coreChunks.begin(), session.coreChunks.end());
    }

    for (const glm::ivec3& chunkPos : session.cachedVisibleChunks) {
        if (nextVisibleChunks.count(chunkPos) == 0) {
            const uint32_t revision = world_.getVoxelWorld().isChunkLoaded(chunkPos) ? world_.getChunk(chunkPos).getRevision() : 0;
            queueChunkUpdate(session, buildUnloadChunkUpdate(chunkPos, revision));
        }
    }

    for (const glm::ivec3& chunkPos : nextVisibleChunks) {
        if (session.cachedVisibleChunks.count(chunkPos) == 0 && world_.getVoxelWorld().isChunkLoaded(chunkPos)) {
            queueChunkUpdate(session, buildUpsertChunkUpdate(chunkPos));
        }
    }

    session.cachedVisibleChunks = std::move(nextVisibleChunks);
}

NetEntitySnapshot GameServer::buildEntitySnapshot(Session& session) {
    MW_PROFILE_SCOPE("Server.BuildEntitySnapshot");

    NetEntitySnapshot snapshot;
    snapshot.sequence = ++session.entitySnapshotSequence;

    updateSessionVisibleChunks(session);
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
    session.pendingChunkUpdates[chunkPos] = std::move(update);
}

void GameServer::updateVisibleChunks(float deltaTime) {
    MW_PROFILE_SCOPE("Server.UpdateVisibleChunks");

    std::unordered_set<glm::ivec3> desiredChunks;

    auto& registry = world_.getActorWorld().registry();

    {
        auto view = registry.view<SessionComponent, TransformComponent>();
        for (auto entity : view) {
            const glm::ivec3 entityChunk = world_.getActorWorld().getEntityChunk(entity);

            forEachChunkInCylinder(entityChunk, AppConfig::instance().chunkViewRadiusHorizontal, AppConfig::instance().chunkViewRadiusVertical, [&](glm::ivec3 chunkPos) {
                if (world_.isChunkInBounds(chunkPos)) {
                    desiredChunks.insert(chunkPos);
                }
            });
        }
    }

    {
        auto view = registry.view<RobotComponent, TransformComponent>();
        for (auto entity : view) {
            const glm::ivec3 entityChunk = world_.getActorWorld().getEntityChunk(entity);

            forEachChunkInBox(entityChunk, kRobotChunkViewRadius, [&](glm::ivec3 chunkPos) {
                if (world_.isChunkInBounds(chunkPos)) {
                    desiredChunks.insert(chunkPos);
                }
            });
        }
    }

    for (const glm::ivec3& chunkPos : desiredChunks) {
        chunkUnloadTimers_.erase(chunkPos);
        loadChunk(chunkPos);
    }

    std::vector<glm::ivec3> chunksToUnload;
    world_.getVoxelWorld().forEachLoadedChunk([&](glm::ivec3 chunkPos) {
        if (desiredChunks.find(chunkPos) == desiredChunks.end()) {
            float& timer = chunkUnloadTimers_[chunkPos];
            timer += deltaTime;
            if (timer >= kChunkUnloadDelaySeconds) {
                chunksToUnload.push_back(chunkPos);
            }
        }
    });

    for (const glm::ivec3& chunkPos : chunksToUnload) {
        chunkUnloadTimers_.erase(chunkPos);
        unloadChunk(chunkPos);
    }

    size_t loadedChunkCount = 0;
    world_.getVoxelWorld().forEachLoadedChunk([&](glm::ivec3) {
        ++loadedChunkCount;
    });

    MW_PROFILE_GAUGE("Server.LoadedChunks", static_cast<double>(loadedChunkCount));
    MW_PROFILE_GAUGE("Server.DesiredChunks", static_cast<double>(desiredChunks.size()));
    MW_PROFILE_GAUGE("Server.PendingUnloadChunks", static_cast<double>(chunkUnloadTimers_.size()));
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
    updateVisibleChunks(0.0f);
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
        loadChunk(chunkPos);
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
    sessionIt->second.lastChunkPos = glm::ivec3(INT_MAX, INT_MAX, INT_MAX);
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
