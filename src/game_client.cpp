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

GameClient::GameClient(RenderContext* renderContext)
    : renderContext_(renderContext) {
    logging::Scope logScope(logging::Channel::Client);

    auto channel = std::make_unique<KcpChannel>(ioContext_, 0);
    const auto serverEndpoint = IPacketChannel::Endpoint(
        asio::ip::make_address(kDefaultServerAddress),
        kDefaultServerPort);
    channel->setRemote(serverEndpoint);
    channel->startHandshake();
    channel_ = std::move(channel);
}

GameClient::~GameClient() = default;

void GameClient::registerSystem(std::unique_ptr<ClientSystem> system) {
    systems_.push_back(std::move(system));
}

void GameClient::update(float deltaTime) {
    logging::Scope logScope(logging::Channel::Client);
    MW_PROFILE_SCOPE("Client.Update");

    pumpNetwork();

    if (!sessionReady_) {
        return;
    }

    replaySnapshots();
    for (auto& system : systems_) {
        system->update(world_, deltaTime);
    }
    sendInputToServer();
}

void GameClient::pumpNetwork() {
    MW_PROFILE_SCOPE("Client.PumpNetwork");

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
        MW_PROFILE_COUNTER("Net.ClientPacketsIn", 1);
        MW_PROFILE_COUNTER("Net.ClientBytesIn", static_cast<int64_t>(packet.size()));

        // Try ServerHello first
        NetServerHello hello;
        if (deserializeServerHello(packet, hello)) {
            handleServerHello(hello);
            continue;
        }

        // Try Snapshot
        NetSnapshot snapshot;
        if (deserializeSnapshot(packet, snapshot)) {
            snapshotBuffer_.push_back(std::move(snapshot));
            continue;
        }

        logging::warn("Ignored unknown packet");
    }
}

void GameClient::handleServerHello(const NetServerHello& hello) {
    if (sessionReady_) {
        logging::warn("Received duplicate ServerHello, ignoring");
        return;
    }

    localSessionId_ = hello.sessionId;
    logging::info("Server assigned session {} with actor '{}'", hello.sessionId, hello.actorName);

    // Create the local player based on server instructions. Spectator is a player mode.
    entt::entity entity = world_.createPlayer(hello.actorName, hello.sessionId, hello.position, hello.playerMode);
    if (entity != entt::null) {
        auto& transform = world_.getActorWorld().registry().get<TransformComponent>(entity);
        transform.rotation.y = hello.yaw;
        transform.rotation.x = hello.pitch;
    }

    // Now register input and render systems with the correct session ID
    auto inputSystemPtr = std::make_unique<InputSystem>(renderContext_, localSessionId_);
    inputSystem_ = inputSystemPtr.get();
    registerSystem(std::move(inputSystemPtr));
    registerSystem(std::make_unique<RenderSystem>(renderContext_, localSessionId_));

    sessionReady_ = true;
}

void GameClient::sendInputToServer() {
    if (!channel_ || helloPending_ || !sessionReady_) {
        return;
    }
    if (!inputSystem_ || !inputSystem_->hasInputChanged()) {
        return;
    }

    auto& registry = world_.getActorWorld().registry();
    auto view = registry.view<SessionComponent, TransformComponent>();
    for (auto entity : view) {
        const auto& session = registry.get<SessionComponent>(entity);
        if (session.sessionId != localSessionId_) {
            continue;
        }
        const auto& transform = registry.get<TransformComponent>(entity);
        NetClientInput input;
        input.position = transform.position;
        input.yaw = transform.rotation.y;
        input.pitch = transform.rotation.x;
        if (registry.all_of<PlayerComponent>(entity)) {
            input.playerMode = registry.get<PlayerComponent>(entity).mode;
        }
        channel_->sendReliable(serializeClientInput(input));
        break;
    }

    inputSystem_->clearInputChanged();
}

void GameClient::replaySnapshots() {
    MW_PROFILE_SCOPE("Client.ReplaySnapshots");

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
    MW_PROFILE_SCOPE("Client.ApplySnapshot");
    MW_PROFILE_COUNTER("Net.SnapshotChunks", static_cast<int64_t>(snapshot.chunks.size()));
    MW_PROFILE_COUNTER("Net.SnapshotBlocks", static_cast<int64_t>(snapshot.blocks.size()));
    MW_PROFILE_COUNTER("Net.SnapshotActors", static_cast<int64_t>(snapshot.actors.size()));

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
            entity = actor.isPlayer
                         ? world_.createRemotePlayer(actor.name, actor.position, actor.playerMode)
                         : world_.createRobot(actor.name, actor.position);
        }
        if (entity == entt::null) {
            continue;
        }
        // Don't overwrite position of local session entities -
        // the client controls them directly via input
        if (registry.all_of<SessionComponent>(entity)) {
            const auto& session = registry.get<SessionComponent>(entity);
            if (session.sessionId == localSessionId_) {
                if (registry.all_of<PlayerComponent>(entity) &&
                    registry.get<PlayerComponent>(entity).mode == PlayerMode::Survival) {
                    auto& transform = registry.get<TransformComponent>(entity);
                    transform.position.y = actor.position.y;
                    if (registry.all_of<PhysicsComponent>(entity)) {
                        registry.get<PhysicsComponent>(entity).velocity = actor.velocity;
                    }
                }
                continue;
            }
        }
        if (actor.isPlayer) {
            world_.getActorWorld().setPlayerMode(entity, actor.playerMode);
        }
        auto& transform = registry.get<TransformComponent>(entity);
        transform.position = actor.position;
        transform.rotation.x = actor.pitch;
        transform.rotation.y = actor.yaw;
        if (registry.all_of<PhysicsComponent>(entity)) {
            auto& physics = registry.get<PhysicsComponent>(entity);
            physics.velocity = actor.velocity;
        }
    }
}
