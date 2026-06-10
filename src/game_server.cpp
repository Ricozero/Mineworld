#include "game_server.h"

#include <cmath>
#include <glm/gtx/hash.hpp>
#include <unordered_set>
#include <utility>

#include "chunk.h"
#include "config.h"
#include "entity.h"
#include "log.h"
#include "net_kcp.h"
#include "profiler.h"
#include "server_system.h"

namespace {

constexpr size_t kMaxBlocksPerSnapshot = 4096;

}  // namespace

GameServer::GameServer() {
    logging::Scope logScope(logging::Channel::Server);

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
    server_ = std::move(kcpServer);

    registerSystem(std::make_unique<PhysicsSystem>());
}

GameServer::~GameServer() = default;

void GameServer::registerSystem(std::unique_ptr<ServerSystem> system) {
    systems_.push_back(std::move(system));
}

void GameServer::update(float deltaTime) {
    logging::Scope logScope(logging::Channel::Server);
    MW_PROFILE_SCOPE("Server.Update");

    pumpNetwork();
    for (auto& system : systems_) {
        system->update(world_, deltaTime);
    }
    updateVisibleChunks();

    const float snapshotInterval = 1.0f / static_cast<float>(AppConfig::instance().ticksPerSecond);
    for (auto& [sessionId, session] : sessions_) {
        if (!session.helloReceived) {
            continue;
        }
        session.snapshotTimer += deltaTime;
        if (session.snapshotTimer < snapshotInterval) {
            continue;
        }
        session.snapshotTimer -= snapshotInterval;

        NetSnapshot snapshot;
        snapshot = buildSnapshot(session, !session.initialSnapshotSent);
        session.initialSnapshotSent = true;

        std::vector<uint8_t> payload;
        payload = serializeSnapshot(snapshot, session.snapshotBuilder);
        MW_PROFILE_COUNTER("Server.ServerSnapshotsOut", 1);
        MW_PROFILE_COUNTER("Server.ServerBytesOut", static_cast<int64_t>(payload.size()));
        server_->sendTo(sessionId, payload);
    }
}

entt::entity GameServer::createLocalPlayer(const std::string& name, uint32_t sessionId, glm::vec3 position, PlayerMode mode) {
    entt::entity entity = world_.createLocalPlayer(name, sessionId, position, mode);
    updateVisibleChunks();
    return entity;
}

entt::entity GameServer::createRobot(const std::string& name, glm::vec3 position) {
    entt::entity entity = world_.createRobot(name, position);
    updateVisibleChunks();
    return entity;
}

bool GameServer::loadChunk(glm::ivec3 chunkPos) {
    if (!world_.loadChunk(chunkPos)) {
        return false;
    }

    for (auto& [sessionId, session] : sessions_) {
        if (session.initialSnapshotSent) {
            session.pendingChunkUpdates.push_back(NetChunkState{chunkPos, true});
            session.pendingDirtyChunks.push_back(chunkPos);
        }
    }
    return true;
}

bool GameServer::unloadChunk(glm::ivec3 chunkPos) {
    if (!world_.unloadChunk(chunkPos)) {
        return false;
    }

    for (auto& [sessionId, session] : sessions_) {
        if (session.initialSnapshotSent) {
            session.pendingChunkUpdates.push_back(NetChunkState{chunkPos, false});
        }
    }
    return true;
}

void GameServer::setBlock(glm::ivec3 worldPos, BlockData blockData) {
    world_.setBlock(worldPos, blockData);

    for (auto& [sessionId, session] : sessions_) {
        if (session.initialSnapshotSent) {
            session.pendingBlockUpdates.push_back(NetBlockState{worldPos, blockData});
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
    auto sessionView = registry.view<SessionComponent, TransformComponent>();

    glm::ivec3 currentChunkPos{INT_MAX, INT_MAX, INT_MAX};
    for (auto entity : sessionView) {
        const auto& sessionComp = registry.get<SessionComponent>(entity);
        if (sessionComp.sessionId != session.sessionId) {
            continue;
        }
        const auto& transform = registry.get<TransformComponent>(entity);
        currentChunkPos = Chunk::worldToChunk(glm::ivec3(
            static_cast<int>(std::floor(transform.position.x)),
            static_cast<int>(std::floor(transform.position.y)),
            static_cast<int>(std::floor(transform.position.z))));
        break;
    }

    if (currentChunkPos.x == INT_MAX) {
        return;
    }

    if (currentChunkPos == session.lastChunkPos) {
        return;
    }

    session.lastChunkPos = currentChunkPos;
    session.cachedVisibleChunks.clear();

    for (int dx = -AppConfig::instance().chunkViewRadius; dx <= AppConfig::instance().chunkViewRadius; ++dx) {
        for (int dy = -AppConfig::instance().chunkViewRadius; dy <= AppConfig::instance().chunkViewRadius; ++dy) {
            for (int dz = -AppConfig::instance().chunkViewRadius; dz <= AppConfig::instance().chunkViewRadius; ++dz) {
                const glm::ivec3 chunkPos = currentChunkPos + glm::ivec3(dx, dy, dz);
                if (world_.isChunkInBounds(chunkPos)) {
                    session.cachedVisibleChunks.insert(chunkPos);
                }
            }
        }
    }
}

NetSnapshot GameServer::buildSnapshot(Session& session, bool forceFullChunkState) {
    MW_PROFILE_SCOPE("Server.BuildSnapshot");

    NetSnapshot snapshot;
    snapshot.sequence = ++session.snapshotSequence;

    auto& registry = world_.getActorWorld().registry();
    auto view = registry.view<NameComponent, TransformComponent>();
    snapshot.actors.reserve(view.size_hint());
    for (auto entity : view) {
        const auto& name = registry.get<NameComponent>(entity);
        const auto& transform = registry.get<TransformComponent>(entity);
        glm::vec3 velocity{0.0f};
        if (registry.all_of<PhysicsComponent>(entity)) {
            velocity = registry.get<PhysicsComponent>(entity).velocity;
        }
        const bool isPlayer = registry.all_of<PlayerComponent>(entity);
        const PlayerMode playerMode = isPlayer ? registry.get<PlayerComponent>(entity).mode : PlayerMode::Survival;
        uint32_t lastInputSequence = 0;
        if (isPlayer && registry.all_of<SessionComponent>(entity)) {
            const auto& sessionComp = registry.get<SessionComponent>(entity);
            if (auto sessionIt = sessions_.find(sessionComp.sessionId); sessionIt != sessions_.end()) {
                lastInputSequence = sessionIt->second.lastProcessedInputSequence;
            }
        }
        snapshot.actors.push_back(NetActorState{
            name.name,
            transform.position,
            velocity,
            transform.rotation.y,
            transform.rotation.x,
            isPlayer,
            playerMode,
            lastInputSequence,
        });
    }

    updateSessionVisibleChunks(session);
    const auto& visibleChunks = session.cachedVisibleChunks;

    if (forceFullChunkState) {
        snapshot.chunks.reserve(visibleChunks.size());
        for (const auto& chunkPos : visibleChunks) {
            if (world_.getVoxelWorld().isChunkLoaded(chunkPos)) {
                snapshot.chunks.push_back(NetChunkState{chunkPos, true});
            }
        }
    } else {
        std::vector<NetChunkState> remainingChunks;
        remainingChunks.reserve(session.pendingChunkUpdates.size());
        for (const auto& chunkState : session.pendingChunkUpdates) {
            if (visibleChunks.count(chunkState.chunkPos) > 0) {
                snapshot.chunks.push_back(chunkState);
            } else {
                remainingChunks.push_back(chunkState);
            }
        }
        session.pendingChunkUpdates = std::move(remainingChunks);
    }

    if (forceFullChunkState && session.pendingBlockUpdates.empty() && session.pendingDirtyChunks.empty()) {
        for (const auto& chunkPos : visibleChunks) {
            if (world_.getVoxelWorld().isChunkLoaded(chunkPos)) {
                session.pendingDirtyChunks.push_back(chunkPos);
            }
        }
    }

    // Expand dirty chunks into block updates incrementally
    while (!session.pendingDirtyChunks.empty() && session.pendingBlockUpdates.size() < kMaxBlocksPerSnapshot * 2) {
        glm::ivec3 chunkPos = session.pendingDirtyChunks.back();
        session.pendingDirtyChunks.pop_back();

        if (!world_.getVoxelWorld().isChunkLoaded(chunkPos)) {
            continue;
        }
        if (visibleChunks.count(chunkPos) == 0) {
            continue;
        }

        queueChunkBlockSnapshot(chunkPos, session);
    }

    // Process pending block updates with in-place compaction
    snapshot.blocks.reserve(std::min(session.pendingBlockUpdates.size(), kMaxBlocksPerSnapshot));

    size_t readIdx = 0;
    size_t writeIdx = 0;
    const size_t totalPending = session.pendingBlockUpdates.size();

    while (readIdx < totalPending && snapshot.blocks.size() < kMaxBlocksPerSnapshot) {
        NetBlockState& block = session.pendingBlockUpdates[readIdx];
        const glm::ivec3 chunkPos = Chunk::worldToChunk(block.worldPos);

        if (!world_.getVoxelWorld().isChunkLoaded(chunkPos)) {
            ++readIdx;
            continue;
        }

        if (visibleChunks.count(chunkPos) > 0) {
            snapshot.blocks.push_back(std::move(block));
            ++readIdx;
        } else {
            if (writeIdx != readIdx) {
                session.pendingBlockUpdates[writeIdx] = std::move(block);
            }
            ++writeIdx;
            ++readIdx;
        }
    }

    while (readIdx < totalPending) {
        if (writeIdx != readIdx) {
            session.pendingBlockUpdates[writeIdx] = std::move(session.pendingBlockUpdates[readIdx]);
        }
        ++writeIdx;
        ++readIdx;
    }

    while (session.pendingBlockUpdates.size() > writeIdx) {
        session.pendingBlockUpdates.pop_back();
    }

    return snapshot;
}

void GameServer::updateVisibleChunks() {
    MW_PROFILE_SCOPE("Server.UpdateVisibleChunks");

    std::unordered_set<glm::ivec3> desiredChunks;

    auto& registry = world_.getActorWorld().registry();

    {
        auto view = registry.view<SessionComponent, TransformComponent>();
        for (auto entity : view) {
            const auto& transform = registry.get<TransformComponent>(entity);
            const glm::ivec3 entityChunk = Chunk::worldToChunk(glm::ivec3(
                static_cast<int>(std::floor(transform.position.x)),
                static_cast<int>(std::floor(transform.position.y)),
                static_cast<int>(std::floor(transform.position.z))));

            for (int dx = -AppConfig::instance().chunkViewRadius; dx <= AppConfig::instance().chunkViewRadius; ++dx) {
                for (int dy = -AppConfig::instance().chunkViewRadius; dy <= AppConfig::instance().chunkViewRadius; ++dy) {
                    for (int dz = -AppConfig::instance().chunkViewRadius; dz <= AppConfig::instance().chunkViewRadius; ++dz) {
                        const glm::ivec3 chunkPos = entityChunk + glm::ivec3(dx, dy, dz);
                        if (world_.isChunkInBounds(chunkPos)) {
                            desiredChunks.insert(chunkPos);
                        }
                    }
                }
            }
        }
    }

    {
        auto view = registry.view<RobotComponent, TransformComponent>();
        for (auto entity : view) {
            const auto& transform = registry.get<TransformComponent>(entity);
            const glm::ivec3 entityChunk = Chunk::worldToChunk(glm::ivec3(
                static_cast<int>(std::floor(transform.position.x)),
                static_cast<int>(std::floor(transform.position.y)),
                static_cast<int>(std::floor(transform.position.z))));

            for (int dx = -AppConfig::instance().chunkViewRadius; dx <= AppConfig::instance().chunkViewRadius; ++dx) {
                for (int dy = -AppConfig::instance().chunkViewRadius; dy <= AppConfig::instance().chunkViewRadius; ++dy) {
                    for (int dz = -AppConfig::instance().chunkViewRadius; dz <= AppConfig::instance().chunkViewRadius; ++dz) {
                        const glm::ivec3 chunkPos = entityChunk + glm::ivec3(dx, dy, dz);
                        if (world_.isChunkInBounds(chunkPos)) {
                            desiredChunks.insert(chunkPos);
                        }
                    }
                }
            }
        }
    }

    for (const glm::ivec3& chunkPos : desiredChunks) {
        loadChunk(chunkPos);
    }

    // Collect chunks to unload before mutating the map
    std::vector<glm::ivec3> chunksToUnload;
    world_.getVoxelWorld().forEachLoadedChunk([&](glm::ivec3 chunkPos) {
        if (desiredChunks.find(chunkPos) == desiredChunks.end()) {
            chunksToUnload.push_back(chunkPos);
        }
    });

    for (const glm::ivec3& chunkPos : chunksToUnload) {
        unloadChunk(chunkPos);
    }
}

void GameServer::queueChunkBlockSnapshot(glm::ivec3 chunkPos, Session& session) {
    MW_PROFILE_SCOPE("Server.QueueChunkBlocks");

    const Chunk& chunk = world_.getChunk(chunkPos);
    for (int x = 0; x < Chunk::SIZE; ++x) {
        for (int y = 0; y < Chunk::SIZE; ++y) {
            for (int z = 0; z < Chunk::SIZE; ++z) {
                const glm::ivec3 localPos(x, y, z);
                const BlockData block = chunk.getBlock(localPos);
                if (block.type == BlockType::Air) {
                    continue;
                }

                session.pendingBlockUpdates.push_back(NetBlockState{chunk.localToWorld(localPos), block});
            }
        }
    }
}

void GameServer::pumpNetwork() {
    MW_PROFILE_SCOPE("Server.PumpNetwork");

    if (!server_) {
        return;
    }

    server_->pump();
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
    updateVisibleChunks();
}

bool GameServer::onSessionPacket(uint32_t sessionId, const std::vector<uint8_t>& packet) {
    MW_PROFILE_COUNTER("Server.ServerPacketsIn", 1);
    MW_PROFILE_COUNTER("Server.ServerBytesIn", static_cast<int64_t>(packet.size()));

    if (deserializeClientHello(packet)) {
        onClientHello(sessionId);
        return true;
    }

    if (deserializeClientDisconnect(packet)) {
        return false;
    }

    NetClientInput input;
    if (deserializeClientInput(packet, input)) {
        onClientInput(sessionId, input);
        return true;
    }

    logging::warn("Ignored unknown client packet from session {}", sessionId);
    return true;
}

void GameServer::onClientHello(uint32_t sessionId) {
    auto& session = getOrCreateSession(sessionId);
    if (session.helloReceived) {
        logging::warn("Duplicate ClientHello from session {}, ignoring", sessionId);
        return;
    }
    session.helloReceived = true;

    std::string actorName = "Player" + std::to_string(nextPlayerIndex_++);
    session.actorName = actorName;

    glm::vec3 spawnPos = AppConfig::instance().spawnPosition;
    float spawnYaw = AppConfig::instance().spawnYaw;
    float spawnPitch = AppConfig::instance().spawnPitch;

    createLocalPlayer(actorName, sessionId, spawnPos, PlayerMode::Survival);

    auto& registry = world_.getActorWorld().registry();
    auto view = registry.view<SessionComponent, TransformComponent>();
    for (auto entity : view) {
        const auto& sessionComp = registry.get<SessionComponent>(entity);
        if (sessionComp.sessionId == sessionId) {
            auto& transform = registry.get<TransformComponent>(entity);
            transform.rotation.y = spawnYaw;
            transform.rotation.x = spawnPitch;
            break;
        }
    }

    NetServerHello hello;
    hello.sessionId = sessionId;
    hello.actorName = actorName;
    hello.position = spawnPos;
    hello.yaw = spawnYaw;
    hello.pitch = spawnPitch;
    hello.playerMode = PlayerMode::Survival;
    server_->sendTo(sessionId, serializeServerHello(hello));

    logging::info("Client hello from session {}, assigned actor '{}'", sessionId, actorName);
}

void GameServer::onClientInput(uint32_t sessionId, const NetClientInput& input) {
    auto sessionIt = sessions_.find(sessionId);
    if (sessionIt != sessions_.end() && input.sequence <= sessionIt->second.lastProcessedInputSequence) {
        return;
    }

    constexpr float kMaxDeltaTime = 0.1f;
    const float deltaTime = std::clamp(input.deltaTime, 0.0f, kMaxDeltaTime);

    auto& registry = world_.getActorWorld().registry();
    auto view = registry.view<SessionComponent, TransformComponent, ControllerInputComponent>();
    for (auto entity : view) {
        const auto& session = registry.get<SessionComponent>(entity);
        if (session.sessionId != sessionId) {
            continue;
        }
        world_.getActorWorld().setPlayerMode(entity, input.playerMode);
        auto& transform = registry.get<TransformComponent>(entity);
        auto& controllerInput = registry.get<ControllerInputComponent>(entity);
        controllerInput.move = input.move;
        if (glm::dot(controllerInput.move, controllerInput.move) > 1.0f) {
            controllerInput.move = glm::normalize(controllerInput.move);
        }
        controllerInput.jump = input.jump;
        controllerInput.sprint = input.sprint;
        controllerInput.sequence = input.sequence;
        controllerInput.deltaTime = deltaTime;
        transform.rotation.x = input.pitch;
        transform.rotation.y = input.yaw;
        if (sessionIt != sessions_.end()) {
            sessionIt->second.lastProcessedInputSequence = input.sequence;
        }
        applyControllerInput(registry, entity, deltaTime, true);
        simulateServerActor(world_, registry, entity, deltaTime);
        break;
    }
}
