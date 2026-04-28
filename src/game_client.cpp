#include "game_client.h"

#include <spdlog/spdlog.h>

#include "entity.h"
#include "net_kcp.h"
#include "system.h"

GameClient::GameClient() {
    auto channel = std::make_unique<KcpChannel>(ioContext_, DEFAULT_CLIENT_PORT, DEFAULT_CONV);
    const auto serverEndpoint = IPacketChannel::Endpoint(
        asio::ip::make_address("127.0.0.1"),
        DEFAULT_SERVER_PORT);
    channel->setRemote(serverEndpoint);
    channel->sendReliable(serializeClientHello());
    channel_ = std::move(channel);

    registerSystem(std::make_unique<InputSystem>());
    registerSystem(std::make_unique<RenderSystem>());
}

GameClient::~GameClient() = default;

void GameClient::registerSystem(std::unique_ptr<ClientSystem> system) {
    systems_.push_back(std::move(system));
}

void GameClient::update(float deltaTime) {
    pumpNetwork();
    replaySnapshots();

    for (auto& system : systems_) {
        system->update(world_, deltaTime);
    }
}

void GameClient::pumpNetwork() {
    if (!channel_) {
        return;
    }
    channel_->pump();

    std::vector<uint8_t> packet;
    while (channel_->popPacket(packet)) {
        NetSnapshot snapshot;
        if (deserializeSnapshot(packet, snapshot)) {
            snapshotBuffer_.push_back(std::move(snapshot));
        } else {
            spdlog::warn("Client ignored unknown packet");
        }
    }
}

void GameClient::replaySnapshots() {
    if (snapshotBuffer_.empty()) {
        return;
    }

    NetSnapshot snapshot = std::move(snapshotBuffer_.front());
    snapshotBuffer_.pop_front();
    if (snapshot.sequence <= lastAppliedSnapshot_) {
        return;
    }

    applySnapshot(snapshot);
    lastAppliedSnapshot_ = snapshot.sequence;
}

void GameClient::applySnapshot(const NetSnapshot& snapshot) {
    for (const auto& chunk : snapshot.chunks) {
        if (chunk.loaded) {
            world_.loadChunk(chunk.chunkPos);
        } else {
            world_.unloadChunk(chunk.chunkPos);
        }
    }

    for (const auto& block : snapshot.blocks) {
        world_.applyBlockSnapshot(block.worldPos, block.data);
    }

    auto& registry = world_.getActorWorld().registry();
    for (const auto& actor : snapshot.actors) {
        entt::entity entity = world_.getEntityByName(actor.name);
        if (entity == entt::null) {
            entity = world_.createPlayer(actor.name, actor.position);
        }
        if (entity == entt::null) {
            continue;
        }
        auto& transform = registry.get<TransformComponent>(entity);
        auto& physics = registry.get<PhysicsComponent>(entity);
        transform.position = actor.position;
        physics.velocity = actor.velocity;
    }
}
