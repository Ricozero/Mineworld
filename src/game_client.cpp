#include "game_client.h"

#include "entity.h"
#include "log.h"
#include "net_kcp.h"
#include "profiler.h"
#include "render_context.h"
#include "system.h"

namespace {

constexpr uint16_t kDefaultServerPort = 40000;
constexpr const char* kDefaultServerAddress = "127.0.0.1";

}  // namespace

GameClient::GameClient(RenderContext* renderContext, const std::string& spectatorName)
    : spectatorName_(spectatorName), renderContext_(renderContext) {
    logging::Scope logScope(logging::Channel::Client);

    auto channel = std::make_unique<KcpChannel>(ioContext_, 0);
    const auto serverEndpoint = IPacketChannel::Endpoint(
        asio::ip::make_address(kDefaultServerAddress),
        kDefaultServerPort);
    channel->setRemote(serverEndpoint);
    channel->startHandshake();
    channel_ = std::move(channel);

    registerSystem(std::make_unique<InputSystem>(renderContext_, spectatorName_));
    registerSystem(std::make_unique<RenderSystem>(renderContext_));
}

GameClient::~GameClient() = default;

void GameClient::registerSystem(std::unique_ptr<ClientSystem> system) {
    systems_.push_back(std::move(system));
}

void GameClient::update(float deltaTime) {
    logging::Scope logScope(logging::Channel::Client);
    profiling::ScopedTimer timer("Client.Update");

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

    // Send ClientHello once handshake is complete
    auto* kcpChannel = dynamic_cast<KcpChannel*>(channel_.get());
    if (helloPending_ && kcpChannel && kcpChannel->isReady()) {
        channel_->sendReliable(serializeClientHello());
        helloPending_ = false;
    }

    std::vector<uint8_t> packet;
    while (channel_->popPacket(packet)) {
        NetSnapshot snapshot;
        if (deserializeSnapshot(packet, snapshot)) {
            snapshotBuffer_.push_back(std::move(snapshot));
        } else {
            logging::warn("Ignored unknown packet");
        }
    }
}

// ... replaySnapshots() and applySnapshot() unchanged ...
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
            if (renderContext_) {
                renderContext_->invalidateChunkCache(chunk.chunkPos);
            }
        } else {
            world_.unloadChunk(chunk.chunkPos);
            if (renderContext_) {
                renderContext_->invalidateChunkCache(chunk.chunkPos);
            }
        }
    }

    for (const auto& block : snapshot.blocks) {
        world_.applyBlockSnapshot(block.worldPos, block.data);
    }

    auto& registry = world_.getActorWorld().registry();
    for (const auto& actor : snapshot.actors) {
        entt::entity entity = world_.getEntityByName(actor.name);
        if (entity == entt::null) {
            if (actor.name == spectatorName_) {
                entity = world_.createSpectator(actor.name, actor.position);
            } else {
                entity = world_.createPlayer(actor.name, actor.position);
            }
        }
        if (entity == entt::null) {
            continue;
        }
        // Don't overwrite spectator position from server snapshot -
        // the client controls it directly via camera
        if (registry.all_of<SpectatorComponent>(entity)) {
            continue;
        }
        auto& transform = registry.get<TransformComponent>(entity);
        auto& physics = registry.get<PhysicsComponent>(entity);
        transform.position = actor.position;
        physics.velocity = actor.velocity;
    }
}