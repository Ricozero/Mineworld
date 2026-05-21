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

constexpr int kChunkViewRadius = 2;
constexpr size_t kMaxBlocksPerSnapshot = 4096;

}  // namespace

GameServer::GameServer() {
    logging::Scope logScope(logging::Channel::Server);

    channel_ = std::make_unique<KcpChannel>(ioContext_, DEFAULT_SERVER_PORT, DEFAULT_CONV);
    registerSystem(std::make_unique<PhysicsSystem>());
}

GameServer::~GameServer() = default;

void GameServer::registerSystem(std::unique_ptr<ServerSystem> system) {
    systems_.push_back(std::move(system));
}

void GameServer::update(float deltaTime) {
    logging::Scope logScope(logging::Channel::Server);
    profiling::ScopedTimer timer("Server.Update");

    {
        profiling::ScopedTimer stepTimer("Server.PumpNetwork");
        pumpNetwork();
    }

    {
        profiling::ScopedTimer stepTimer("Server.Systems");
        for (auto& system : systems_) {
            system->update(world_, deltaTime);
        }
    }

    {
        profiling::ScopedTimer stepTimer("Server.VisibleChunks");
        updateVisibleChunks();
    }

    snapshotTimer_ += deltaTime;
    constexpr float snapshotInterval = 1.0f / 20.0f;
    if (channel_ && channel_->hasRemote() && snapshotTimer_ >= snapshotInterval) {
        snapshotTimer_ -= snapshotInterval;
        NetSnapshot snapshot;
        {
            profiling::ScopedTimer stepTimer("Server.BuildSnapshot");
            snapshot = buildSnapshot(!initialSnapshotSent_);
        }
        initialSnapshotSent_ = true;
        std::vector<uint8_t> payload;
        {
            profiling::ScopedTimer stepTimer("Server.SerializeSnapshot");
            payload = serializeSnapshot(snapshot);
        }
        {
            profiling::ScopedTimer stepTimer("Server.SendSnapshot");
            channel_->sendReliable(payload);
        }
    }
}

entt::entity GameServer::createPlayer(const std::string& name, glm::vec3 position) {
    entt::entity entity = world_.createPlayer(name, position);
    updateVisibleChunks();
    return entity;
}

entt::entity GameServer::createSpectator(const std::string& name, glm::vec3 position) {
    entt::entity entity = world_.createSpectator(name, position);
    updateVisibleChunks();
    return entity;
}

bool GameServer::loadChunk(glm::ivec3 chunkPos) {
    if (!world_.loadChunk(chunkPos)) {
        return false;
    }
    if (initialSnapshotSent_ && channel_ && channel_->hasRemote()) {
        pendingChunkUpdates_.push_back(NetChunkState{chunkPos, true});
        queueChunkBlockSnapshot(chunkPos);
    }
    return true;
}

bool GameServer::unloadChunk(glm::ivec3 chunkPos) {
    if (!world_.unloadChunk(chunkPos)) {
        return false;
    }
    if (initialSnapshotSent_ && channel_ && channel_->hasRemote()) {
        pendingChunkUpdates_.push_back(NetChunkState{chunkPos, false});
    }
    return true;
}

void GameServer::setBlock(glm::ivec3 worldPos, BlockData blockData) {
    world_.setBlock(worldPos, blockData);
    if (initialSnapshotSent_ && channel_ && channel_->hasRemote()) {
        pendingBlockUpdates_.push_back(NetBlockState{worldPos, blockData});
    }
}

NetSnapshot GameServer::buildSnapshot(bool forceFullChunkState) {
    NetSnapshot snapshot;
    snapshot.sequence = ++snapshotSequence_;

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

    if (forceFullChunkState) {
        auto loadedChunks = world_.getLoadedChunks();
        snapshot.chunks.reserve(loadedChunks.size());
        for (const auto& chunkPos : loadedChunks) {
            snapshot.chunks.push_back(NetChunkState{chunkPos, true});
        }
    } else {
        snapshot.chunks = pendingChunkUpdates_;
    }

    if (forceFullChunkState && pendingBlockUpdates_.empty()) {
        queueLoadedBlockSnapshots();
    }

    snapshot.blocks.reserve(kMaxBlocksPerSnapshot);
    while (!pendingBlockUpdates_.empty() && snapshot.blocks.size() < kMaxBlocksPerSnapshot) {
        NetBlockState block = pendingBlockUpdates_.front();
        pendingBlockUpdates_.pop_front();
        if (!world_.getVoxelWorld().isChunkLoaded(Chunk::worldToChunk(block.worldPos))) {
            continue;
        }
        snapshot.blocks.push_back(block);
    }

    pendingChunkUpdates_.clear();
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

void GameServer::queueChunkBlockSnapshot(glm::ivec3 chunkPos) {
    const Chunk& chunk = world_.getChunk(chunkPos);
    for (int x = 0; x < Chunk::SIZE; ++x) {
        for (int y = 0; y < Chunk::SIZE; ++y) {
            for (int z = 0; z < Chunk::SIZE; ++z) {
                const glm::ivec3 localPos(x, y, z);
                const BlockData block = chunk.getBlock(localPos);
                if (block.type == BlockType::Air) {
                    continue;
                }
                pendingBlockUpdates_.push_back(NetBlockState{chunk.localToWorld(localPos), block});
            }
        }
    }
}

void GameServer::queueLoadedBlockSnapshots() {
    for (const glm::ivec3& chunkPos : world_.getLoadedChunks()) {
        queueChunkBlockSnapshot(chunkPos);
    }
}

void GameServer::pumpNetwork() {
    if (!channel_) {
        return;
    }
    channel_->pump();

    std::vector<uint8_t> packet;
    while (channel_->popPacket(packet)) {
        onClientPacket(packet);
    }
}

void GameServer::onClientPacket(const std::vector<uint8_t>& packet) {
    if (deserializeClientHello(packet)) {
        logging::info("Client hello received, remote endpoint bound");
        return;
    }
    logging::warn("Ignored unknown client packet");
}