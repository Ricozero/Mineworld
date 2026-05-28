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
    profiling::ScopedTimer timer("Server.Update");

    pumpNetwork();
    for (auto& system : systems_) {
        system->update(world_, deltaTime);
    }
    updateVisibleChunks();

    constexpr float snapshotInterval = 1.0f / kTicksPerSecond;
    for (auto& [sessionId, session] : sessions_) {
        session.snapshotTimer += deltaTime;
        if (session.snapshotTimer < snapshotInterval) {
            continue;
        }
        session.snapshotTimer -= snapshotInterval;

        NetSnapshot snapshot;
        snapshot = buildSnapshot(session, !session.initialSnapshotSent);
        session.initialSnapshotSent = true;

        std::vector<uint8_t> payload;
        payload = serializeSnapshot(snapshot);
        server_->sendTo(sessionId, payload);
    }
}

entt::entity GameServer::createPlayer(const std::string& name, glm::vec3 position) {
    entt::entity entity = world_.createPlayer(name, position);
    updateVisibleChunks();
    return entity;
}

entt::entity GameServer::createSpectator(const std::string& name, uint32_t sessionId, glm::vec3 position) {
    entt::entity entity = world_.createSpectator(name, position);
    auto& registry = world_.getActorWorld().registry();

    registry.emplace<SessionComponent>(entity, sessionId);
    updateVisibleChunks();
    return entity;
}

bool GameServer::loadChunk(glm::ivec3 chunkPos) {
    if (!world_.loadChunk(chunkPos)) {
        return false;
    }

    // Notify all sessions about the chunk load
    for (auto& [sessionId, session] : sessions_) {
        if (session.initialSnapshotSent) {
            session.pendingChunkUpdates.push_back(NetChunkState{chunkPos, true});
            queueChunkBlockSnapshot(chunkPos, session);
        }
    }
    return true;
}

bool GameServer::unloadChunk(glm::ivec3 chunkPos) {
    if (!world_.unloadChunk(chunkPos)) {
        return false;
    }

    // Notify all sessions about the chunk unload
    for (auto& [sessionId, session] : sessions_) {
        if (session.initialSnapshotSent) {
            session.pendingChunkUpdates.push_back(NetChunkState{chunkPos, false});
        }
    }
    return true;
}

void GameServer::setBlock(glm::ivec3 worldPos, BlockData blockData) {
    world_.setBlock(worldPos, blockData);

    // Notify all sessions about the block change
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

NetSnapshot GameServer::buildSnapshot(Session& session, bool forceFullChunkState) {
    NetSnapshot snapshot;

    snapshot.sequence = ++session.snapshotSequence;

    auto& registry = world_.getActorWorld().registry();
    auto view = registry.view<NameComponent, TransformComponent, PhysicsComponent>();
    snapshot.actors.reserve(view.size_hint());
    for (auto entity : view) {
        const auto& name = registry.get<NameComponent>(entity);
        const auto& transform = registry.get<TransformComponent>(entity);
        const auto& physics = registry.get<PhysicsComponent>(entity);
        snapshot.actors.push_back(NetActorState{
            name.name,
            transform.position,
            physics.velocity,
        });
    }

    // Filter chunks: only send chunks visible to spectators in this session.
    std::unordered_set<glm::ivec3> visibleChunks;
    auto spectatorView = registry.view<SpectatorComponent, TransformComponent, SessionComponent>();
    for (auto entity : spectatorView) {
        const auto& sessionComp = registry.get<SessionComponent>(entity);
        if (sessionComp.sessionId != session.sessionId) {
            continue;
        }
        const auto& transform = registry.get<TransformComponent>(entity);
        const glm::ivec3 playerChunk = Chunk::worldToChunk(glm::ivec3(
            static_cast<int>(std::floor(transform.position.x)),
            static_cast<int>(std::floor(transform.position.y)),
            static_cast<int>(std::floor(transform.position.z))));

        for (int dx = -kChunkViewRadius; dx <= kChunkViewRadius; ++dx) {
            for (int dy = -kChunkViewRadius; dy <= kChunkViewRadius; ++dy) {
                for (int dz = -kChunkViewRadius; dz <= kChunkViewRadius; ++dz) {
                    const glm::ivec3 chunkPos = playerChunk + glm::ivec3(dx, dy, dz);
                    if (world_.isChunkInBounds(chunkPos)) {
                        visibleChunks.insert(chunkPos);
                    }
                }
            }
        }
    }

    if (forceFullChunkState) {
        auto loadedChunks = world_.getLoadedChunks();
        snapshot.chunks.reserve(visibleChunks.size());
        for (const auto& chunkPos : loadedChunks) {
            if (visibleChunks.count(chunkPos) > 0) {
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

    if (forceFullChunkState && session.pendingBlockUpdates.empty()) {
        queueLoadedBlockSnapshots(session, visibleChunks);
    }

    snapshot.blocks.reserve(kMaxBlocksPerSnapshot);
    std::deque<NetBlockState> remainingBlocks;

    while (!session.pendingBlockUpdates.empty() && snapshot.blocks.size() < kMaxBlocksPerSnapshot) {
        NetBlockState block = session.pendingBlockUpdates.front();
        session.pendingBlockUpdates.pop_front();
        const glm::ivec3 chunkPos = Chunk::worldToChunk(block.worldPos);
        if (!world_.getVoxelWorld().isChunkLoaded(chunkPos)) {
            continue;
        }
        if (visibleChunks.count(chunkPos) > 0) {
            snapshot.blocks.push_back(block);
        } else {
            remainingBlocks.push_back(block);
        }
    }

    while (!session.pendingBlockUpdates.empty()) {
        remainingBlocks.push_back(session.pendingBlockUpdates.front());
        session.pendingBlockUpdates.pop_front();
    }

    session.pendingBlockUpdates = std::move(remainingBlocks);

    return snapshot;
}

void GameServer::updateVisibleChunks() {
    std::unordered_set<glm::ivec3> desiredChunks;

    auto& registry = world_.getActorWorld().registry();

    // Collect chunks around players
    {
        auto view = registry.view<PlayerComponent, TransformComponent>();
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

    // Collect chunks around spectators
    {
        auto view = registry.view<SpectatorComponent, TransformComponent>();
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

    const std::vector<glm::ivec3> loadedChunks = world_.getLoadedChunks();
    for (const glm::ivec3& chunkPos : loadedChunks) {
        if (desiredChunks.find(chunkPos) == desiredChunks.end()) {
            unloadChunk(chunkPos);
        }
    }
}

void GameServer::queueChunkBlockSnapshot(glm::ivec3 chunkPos, Session& session) {
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

void GameServer::queueLoadedBlockSnapshots(Session& session, const std::unordered_set<glm::ivec3>& visibleChunks) {
    if (visibleChunks.empty()) {
        return;
    }

    for (const glm::ivec3& chunkPos : visibleChunks) {
        if (world_.getVoxelWorld().isChunkLoaded(chunkPos)) {
            queueChunkBlockSnapshot(chunkPos, session);
        }
    }
}

void GameServer::pumpNetwork() {
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
    if (deserializeClientHello(packet)) {
        logging::info("Client hello received from session {}", sessionId);
        return;
    }

    logging::warn("Ignored unknown client packet from session {}", sessionId);
}