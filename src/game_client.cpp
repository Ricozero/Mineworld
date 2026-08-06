#include "game_client.h"

#include <algorithm>
#include <unordered_set>

#include "client_system.h"
#include "entity.h"
#include "log.h"
#include "net_kcp.h"
#include "profiler.h"
#include "render_context.h"

namespace {

constexpr float kConnectionTimeoutSeconds = 10.0f;

}

GameClient::GameClient(RenderContext* renderContext, std::string address, uint16_t port)
    : renderContext_(renderContext) {
    auto netClient = std::make_unique<KcpClient>(ioContext_, 0);
    asio::error_code addressError;
    asio::ip::address resolvedAddress = asio::ip::make_address(address, addressError);
    if (addressError) {
        logging::warn("Invalid server address '{}': {}", address, addressError.message());
        resolvedAddress = asio::ip::make_address("127.0.0.1");
    }
    const auto serverEndpoint = INetClient::Endpoint(resolvedAddress, port);
    netClient->connect(serverEndpoint);
    netClient_ = std::move(netClient);
}

GameClient::~GameClient() = default;

void GameClient::registerSystem(std::unique_ptr<common_system::BaseSystem<ClientWorld>> system) {
    systems_.push_back(std::move(system));
}

void GameClient::update(float deltaTime) {
    MW_PROFILE_SCOPE("Client.Update");

    if (state_ == State::Failed || state_ == State::Disconnecting) {
        return;
    }

    secondsSincePacket_ += deltaTime;
    pumpNetwork();
    if (secondsSincePacket_ >= kConnectionTimeoutSeconds) {
        fail("Connection timed out");
        return;
    }

    if (state_ != State::Loading && state_ != State::Running) {
        return;
    }

    if (state_ == State::Loading) {
        if (!areCoreChunksLoaded()) {
            replaySnapshots();
        }
        if (renderContext_) {
            renderContext_->updateCoreChunkMeshes(world_, coreChunks_);
        }
        tryEnterRunning();
        return;
    }

    replaySnapshots();
    updateRemoteInterpolation(deltaTime);
    for (auto& system : systems_) {
        system->update(world_, deltaTime);
    }
    sendInputToServer();
}

std::string GameClient::statusText() const {
    switch (state_) {
        case State::Connecting:
            return "Connecting transport...";
        case State::Awaiting:
            return "Waiting for ServerHello...";
        case State::Loading:
            return "Loading core chunks...";
        case State::Running:
            return "Running";
        case State::Disconnecting:
            return "Disconnecting...";
        case State::Failed:
            return failureReason_.empty() ? "Connection failed" : failureReason_;
    }
    return "Unknown state";
}

void GameClient::pumpNetwork() {
    MW_PROFILE_SCOPE("Client.PumpNetwork");

    if (!netClient_) {
        return;
    }
    netClient_->pump();

    // Send ClientHello once handshake is complete
    if (helloPending_ && netClient_->isReady()) {
        netClient_->sendReliable(serializeClientHello());
        helloPending_ = false;
        state_ = State::Awaiting;
        secondsSincePacket_ = 0.0f;
    }

    std::vector<uint8_t> packet;
    while (netClient_->popPacket(packet)) {
        secondsSincePacket_ = 0.0f;
        onServerPacket(packet);
    }
}

void GameClient::onServerPacket(const std::vector<uint8_t>& packet) {
    MW_PROFILE_COUNTER("Client.PacketsIn", 1);
    MW_PROFILE_COUNTER("Client.BytesIn", static_cast<int64_t>(packet.size()));

    using Payload = mineworld::net::NetMessagePayload;
    switch (getPacketType(packet)) {
        case Payload::ServerHello: {
            NetServerHello hello;
            if (deserializeServerHello(packet, hello)) {
                handleServerHello(hello);
            }
            break;
        }
        case Payload::Snapshot: {
            NetSnapshot snapshot;
            if (deserializeSnapshot(packet, snapshot)) {
                snapshotBuffer_.push_back(std::move(snapshot));
            }
            break;
        }
        default:
            logging::warn("Ignored unknown server packet");
            break;
    }
}

void GameClient::disconnect() {
    if (disconnectSent_ || !netClient_ || helloPending_) {
        return;
    }

    netClient_->sendReliable(serializeClientDisconnect());
    netClient_->flush();
    disconnectSent_ = true;
    state_ = State::Disconnecting;
    logging::info("Requested disconnect from server");
}

void GameClient::handleServerHello(const NetServerHello& hello) {
    if (state_ != State::Awaiting) {
        logging::warn("Ignored ServerHello while client state is {}", static_cast<int>(state_));
        return;
    }
    if (hello.coreChunks.empty()) {
        fail("ServerHello did not include core chunks");
        return;
    }

    localSessionId_ = hello.sessionId;
    coreChunks_ = hello.coreChunks;
    logging::info("Server assigned session {} with actor '{}'", hello.sessionId, hello.actorName);

    entt::entity entity = world_.createLocalPlayer(hello.actorName, hello.sessionId, hello.position, hello.playerMode);
    auto& registry = world_.getActorWorld().registry();
    if (!registry.valid(entity) || !registry.all_of<TransformComponent>(entity)) {
        fail("Failed to create local player");
        return;
    }
    auto& transform = registry.get<TransformComponent>(entity);
    transform.rotation.y = hello.yaw;
    transform.rotation.x = hello.pitch;

    state_ = State::Loading;
}

void GameClient::tryEnterRunning() {
    if (!areCoreChunksLoaded()) {
        return;
    }

    for (const glm::ivec3& chunkPos : coreChunks_) {
        if (renderContext_ && !renderContext_->isChunkMeshReady(chunkPos)) {
            return;
        }
    }

    netClient_->sendReliable(serializeClientReady());
    netClient_->flush();

    registerSystem(std::make_unique<InputSystem>(renderContext_, localSessionId_));
    registerSystem(std::make_unique<RenderSystem>(renderContext_, localSessionId_));

    coreChunks_.clear();
    state_ = State::Running;
    logging::info("World is ready");
}

bool GameClient::areCoreChunksLoaded() const {
    for (const glm::ivec3& chunkPos : coreChunks_) {
        if (!world_.getVoxelWorld().isChunkLoaded(chunkPos)) {
            return false;
        }
    }
    return true;
}

void GameClient::fail(std::string reason) {
    if (state_ == State::Failed) {
        return;
    }
    failureReason_ = std::move(reason);
    state_ = State::Failed;
    if (localSessionId_ != 0) {
        logging::warn("Client disconnected from server (session {}): {}", localSessionId_, failureReason_);
    } else {
        logging::warn("Client connection failed: {}", failureReason_);
    }
}

void GameClient::sendInputToServer() {
    if (!netClient_ || helloPending_ || state_ != State::Running) {
        return;
    }

    auto& registry = world_.getActorWorld().registry();
    auto view = registry.view<SessionComponent, TransformComponent, PlayerComponent>();
    for (auto entity : view) {
        const auto& session = view.get<SessionComponent>(entity);
        if (session.sessionId != localSessionId_) {
            continue;
        }
        const auto& transform = view.get<TransformComponent>(entity);
        const auto& player = view.get<PlayerComponent>(entity);
        NetClientInput input;
        input.position = transform.position;
        if (registry.all_of<PhysicsComponent>(entity)) {
            input.velocity = registry.get<PhysicsComponent>(entity).velocity;
        }
        input.yaw = transform.rotation.y;
        input.pitch = transform.rotation.x;
        input.playerMode = player.mode;
        input.sequence = nextInputSequence_++;
        netClient_->sendReliable(serializeClientInput(input));
        break;
    }
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
    MW_PROFILE_COUNTER("Client.SnapshotChunks", static_cast<int64_t>(snapshot.chunks.size()));
    MW_PROFILE_COUNTER("Client.SnapshotActors", static_cast<int64_t>(snapshot.actors.size()));

    for (const auto& chunk : snapshot.chunks) {
        if (chunk.loaded) {
            if (!world_.applyChunkSnapshot(chunk.chunkPos, chunk.blocks)) {
                logging::warn("Ignoring malformed chunk snapshot at ({}, {}, {}) with {} blocks",
                              chunk.chunkPos.x, chunk.chunkPos.y, chunk.chunkPos.z, chunk.blocks.size());
                continue;
            }
        } else {
            world_.unloadChunk(chunk.chunkPos);
        }
        if (renderContext_) {
            renderContext_->invalidateChunkCache(chunk.chunkPos);
        }
    }

    auto& registry = world_.getActorWorld().registry();
    std::unordered_set<std::string> snapshotActorNames;
    snapshotActorNames.reserve(snapshot.actors.size());
    for (const auto& actor : snapshot.actors) {
        snapshotActorNames.insert(actor.name);
        entt::entity entity = world_.getEntityByName(actor.name);
        if (entity == entt::null) {
            switch (actor.entityType) {
                case EntityType::Player:
                    entity = world_.createRemotePlayer(actor.name, actor.position, actor.playerMode);
                    break;
                case EntityType::Robot:
                    entity = world_.createRobot(actor.name, actor.position);
                    break;
            }
        }
        if (entity == entt::null) {
            continue;
        }
        if (!registry.all_of<SessionComponent>(entity)) {
            if (actor.entityType == EntityType::Player) {
                world_.getActorWorld().setPlayerMode(entity, actor.playerMode);
            }
            queueRemoteActorSample(registry, entity, actor);
        }
    }

    std::vector<entt::entity> remoteActorsToDestroy;
    auto view = registry.view<NameComponent, TransformComponent>(entt::exclude<SessionComponent>);
    for (auto entity : view) {
        const auto& name = view.get<NameComponent>(entity);
        if (snapshotActorNames.count(name.name) == 0) {
            remoteActorsToDestroy.push_back(entity);
        }
    }
    for (entt::entity entity : remoteActorsToDestroy) {
        world_.destroyEntity(entity);
    }
}

void GameClient::queueRemoteActorSample(entt::registry& registry, entt::entity entity, const NetActorState& actor) {
    if (!registry.all_of<InterpolationComponent>(entity)) {
        registry.emplace<InterpolationComponent>(entity);
    }

    auto& interpolation = registry.get<InterpolationComponent>(entity);
    interpolation.samples.push_back(InterpolationSample{
        actor.position,
        glm::vec3(actor.pitch, actor.yaw, 0.0f),
        actor.velocity,
        actor.playerMode,
        snapshotClock_,
    });

    constexpr size_t maxSamples = 8;
    while (interpolation.samples.size() > maxSamples) {
        interpolation.samples.pop_front();
    }
}

void GameClient::updateRemoteInterpolation(float deltaTime) {
    MW_PROFILE_SCOPE("Client.RemoteInterpolation");
    snapshotClock_ += deltaTime;

    constexpr double interpolationDelay = 0.10;
    const double renderTime = snapshotClock_ - interpolationDelay;
    auto& registry = world_.getActorWorld().registry();
    auto view = registry.view<TransformComponent, InterpolationComponent>(entt::exclude<SessionComponent>);
    for (auto entity : view) {
        auto& interpolation = view.get<InterpolationComponent>(entity);
        auto& samples = interpolation.samples;
        if (samples.empty()) {
            continue;
        }

        while (samples.size() >= 2 && samples[1].time <= renderTime) {
            samples.pop_front();
        }

        auto& transform = view.get<TransformComponent>(entity);
        if (samples.size() < 2 || renderTime <= samples.front().time) {
            const auto& sample = samples.front();
            transform.position = sample.position;
            transform.rotation = sample.rotation;
            continue;
        }

        const auto& from = samples[0];
        const auto& to = samples[1];
        const double duration = std::max(to.time - from.time, 0.001);
        const float t = static_cast<float>(std::clamp((renderTime - from.time) / duration, 0.0, 1.0));
        transform.position = glm::mix(from.position, to.position, t);
        transform.rotation = glm::mix(from.rotation, to.rotation, t);
        if (registry.all_of<PhysicsComponent>(entity)) {
            registry.get<PhysicsComponent>(entity).velocity = glm::mix(from.velocity, to.velocity, t);
        }
        if (registry.all_of<PlayerComponent>(entity)) {
            registry.get<PlayerComponent>(entity).mode = to.playerMode;
        }
    }
}
