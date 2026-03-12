#include "game_server.h"

#include <utility>

#include <spdlog/spdlog.h>

#include "entity.h"
#include "system.h"

GameServer::GameServer() {
    channel_ = std::make_unique<KcpChannel>(ioContext_, DEFAULT_SERVER_PORT, DEFAULT_CONV);
    registerSystem(std::make_unique<PhysicsSystem>());
}

GameServer::~GameServer() = default;

void GameServer::registerSystem(std::unique_ptr<BaseSystem> system) {
    systems_.push_back(std::move(system));
}

void GameServer::update(float deltaTime) {
    pumpNetwork();

    for (auto& system : systems_) {
        system->update(world_, deltaTime);
    }

    snapshotTimer_ += deltaTime;
    constexpr float snapshotInterval = 1.0f / 20.0f;
    if (channel_ && channel_->hasRemote() && snapshotTimer_ >= snapshotInterval) {
        snapshotTimer_ -= snapshotInterval;
        NetSnapshot snapshot = buildSnapshot(!initialSnapshotSent_);
        initialSnapshotSent_ = true;
        const std::vector<uint8_t> payload = serializeSnapshot(snapshot);
        channel_->sendReliable(payload);
    }
}

bool GameServer::loadChunk(glm::ivec3 chunkPos) {
    if (!world_.loadChunkServer(chunkPos)) {
        return false;
    }
    pendingChunkUpdates_.push_back(NetChunkState{chunkPos, true});
    return true;
}

bool GameServer::unloadChunk(glm::ivec3 chunkPos) {
    if (!world_.unloadChunkServer(chunkPos)) {
        return false;
    }
    pendingChunkUpdates_.push_back(NetChunkState{chunkPos, false});
    return true;
}

void GameServer::setBlock(glm::ivec3 worldPos, BlockData blockData) {
    world_.setBlockServer(worldPos, blockData);
    pendingBlockUpdates_.push_back(NetBlockState{worldPos, blockData});
}

NetSnapshot GameServer::buildSnapshot(bool forceFullChunkState) {
    NetSnapshot snapshot;
    snapshot.sequence = ++snapshotSequence_;

    auto& registry = world_.getRegistry();
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

    snapshot.blocks = pendingBlockUpdates_;
    pendingChunkUpdates_.clear();
    pendingBlockUpdates_.clear();
    return snapshot;
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
        spdlog::info("Client hello received, remote endpoint bound");
        return;
    }
    spdlog::warn("Server ignored unknown client packet");
}
