#include "game_server.h"

#include <cmath>
#include <glm/gtx/hash.hpp>
#include <unordered_set>
#include <utility>

#include "chunk.h"
#include "entity.h"
#include "log.h"
#include "net_kcp.h"
#include "profiler.h"
#include "system.h"

namespace {

constexpr uint16_t kDefaultServerPort = 40000;
constexpr int kTicksPerSecond = 20;
constexpr int kChunkViewRadius = 2;
constexpr size_t kMaxBlocksPerSnapshot = 4096;

glm::vec3 kDefaultSpawnPosition = glm::vec3(0.0f, 2.0f, 0.0f);
float kDefaultSpawnYaw = -90.0f;
float kDefaultSpawnPitch = -12.0f;

bool intersectsSolidBlock(ServerWorld& world, entt::registry& registry, entt::entity entity) {
    if (!registry.all_of<TransformComponent, BoxColliderComponent>(entity)) {
        return false;
    }

    const auto& transform = registry.get<TransformComponent>(entity);
    const auto& collider = registry.get<BoxColliderComponent>(entity);
    const glm::vec3 halfSize = collider.size * 0.5f;
    const glm::vec3 min = transform.position + collider.offset - halfSize;
    const glm::vec3 max = transform.position + collider.offset + halfSize;
    constexpr float epsilon = 0.001f;

    const glm::ivec3 minBlock{
        static_cast<int>(std::floor(min.x)),
        static_cast<int>(std::floor(min.y)),
        static_cast<int>(std::floor(min.z)),
    };
    const glm::ivec3 maxBlock{
        static_cast<int>(std::floor(max.x - epsilon)),
        static_cast<int>(std::floor(max.y - epsilon)),
        static_cast<int>(std::floor(max.z - epsilon)),
    };

    for (int x = minBlock.x; x <= maxBlock.x; ++x) {
        for (int y = minBlock.y; y <= maxBlock.y; ++y) {
            for (int z = minBlock.z; z <= maxBlock.z; ++z) {
                if (world.getBlock(glm::ivec3(x, y, z)).type != BlockType::Air) {
                    return true;
                }
            }
        }
    }
    return false;
}

}  // namespace

GameServer::GameServer() {
    logging::Scope logScope(logging::Channel::Server);

    auto kcpServer = std::make_unique<KcpServer>(ioContext_, kDefaultServerPort);
    kcpServer->setOnConnect([this](uint32_t sessionId) {
        onSessionConnect(sessionId);
    });
    kcpServer->setOnPacket([this](uint32_t sessionId, const std::vector<uint8_t>& packet) {
        onSessionPacket(sessionId, packet);
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

    constexpr float snapshotInterval = 1.0f / kTicksPerSecond;
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
        MW_PROFILE_COUNTER("Net.ServerSnapshotsOut", 1);
        MW_PROFILE_COUNTER("Net.ServerBytesOut", static_cast<int64_t>(payload.size()));
        server_->sendTo(sessionId, payload);
    }
}

entt::entity GameServer::createPlayer(const std::string& name, uint32_t sessionId, glm::vec3 position, PlayerMode mode) {
    entt::entity entity = world_.createPlayer(name, sessionId, position, mode);
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

    if (currentChunkPos == session.lastChunkPos) {
        return;
    }

    session.lastChunkPos = currentChunkPos;
    session.cachedVisibleChunks.clear();

    if (currentChunkPos.x == INT_MAX) {
        return;
    }

    for (int dx = -kChunkViewRadius; dx <= kChunkViewRadius; ++dx) {
        for (int dy = -kChunkViewRadius; dy <= kChunkViewRadius; ++dy) {
            for (int dz = -kChunkViewRadius; dz <= kChunkViewRadius; ++dz) {
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
        snapshot.actors.push_back(NetActorState{
            name.name,
            transform.position,
            velocity,
            transform.rotation.y,
            transform.rotation.x,
            isPlayer,
            playerMode,
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

            for (int dx = -kChunkViewRadius; dx <= kChunkViewRadius; ++dx) {
                for (int dy = -kChunkViewRadius; dy <= kChunkViewRadius; ++dy) {
                    for (int dz = -kChunkViewRadius; dz <= kChunkViewRadius; ++dz) {
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

            for (int dx = -kChunkViewRadius; dx <= kChunkViewRadius; ++dx) {
                for (int dy = -kChunkViewRadius; dy <= kChunkViewRadius; ++dy) {
                    for (int dz = -kChunkViewRadius; dz <= kChunkViewRadius; ++dz) {
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

void GameServer::onSessionPacket(uint32_t sessionId, const std::vector<uint8_t>& packet) {
    MW_PROFILE_COUNTER("Net.ServerPacketsIn", 1);
    MW_PROFILE_COUNTER("Net.ServerBytesIn", static_cast<int64_t>(packet.size()));

    if (deserializeClientHello(packet)) {
        onClientHello(sessionId);
        return;
    }

    NetClientInput input;
    if (deserializeClientInput(packet, input)) {
        onClientInput(sessionId, input);
        return;
    }

    logging::warn("Ignored unknown client packet from session {}", sessionId);
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

    glm::vec3 spawnPos = kDefaultSpawnPosition;
    float spawnYaw = kDefaultSpawnYaw;
    float spawnPitch = kDefaultSpawnPitch;

    createPlayer(actorName, sessionId, spawnPos, PlayerMode::Survival);

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
    auto& registry = world_.getActorWorld().registry();
    auto view = registry.view<SessionComponent, TransformComponent>();
    for (auto entity : view) {
        const auto& session = registry.get<SessionComponent>(entity);
        if (session.sessionId != sessionId) {
            continue;
        }
        world_.getActorWorld().setPlayerMode(entity, input.playerMode);
        auto& transform = registry.get<TransformComponent>(entity);
        if (input.playerMode == PlayerMode::Spectator) {
            transform.position = input.position;
        } else {
            const glm::vec3 previousPosition = transform.position;
            transform.position.x = input.position.x;
            if (intersectsSolidBlock(world_, registry, entity)) {
                transform.position.x = previousPosition.x;
            }
            transform.position.z = input.position.z;
            if (intersectsSolidBlock(world_, registry, entity)) {
                transform.position.z = previousPosition.z;
            }
        }
        transform.rotation.x = input.pitch;
        transform.rotation.y = input.yaw;
        world_.getActorWorld().updateEntityChunk(entity, transform.position);
        break;
    }
}
