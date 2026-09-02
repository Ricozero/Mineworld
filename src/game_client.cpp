#include "game_client.h"

#include <algorithm>
#include <chrono>
#include <unordered_set>

#include "chunk_mesh.h"
#include "client_system.h"
#include "direction.h"
#include "entity.h"
#include "log.h"
#include "net_kcp.h"
#include "profiler.h"

namespace {

constexpr float kConnectionTimeoutSeconds = 10.0f;
constexpr size_t kMaxChunkMeshRebuildsPerFrame = 128;
constexpr double kMaxChunkMeshRebuildTimePerFrame = 8.0;

}  // namespace

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

void GameClient::registerSystem(std::unique_ptr<System> system) {
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
        replayEntitySnapshots();
        rebuildChunkMeshes();
        tryEnterRunning();
        return;
    }

    replayEntitySnapshots();
    rebuildChunkMeshes();
    updateRemoteInterpolation(deltaTime);
    for (auto& system : systems_) {
        system->update(voxelWorld_, actorWorld_, deltaTime);
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
        case Payload::EntitySnapshot: {
            NetEntitySnapshot snapshot;
            if (deserializeEntitySnapshot(packet, snapshot)) {
                entitySnapshotBuffer_.push_back(std::move(snapshot));
            }
            break;
        }
        case Payload::ChunkUpdate: {
            NetChunkUpdate update;
            if (deserializeChunkUpdate(packet, update)) {
                applyChunkUpdate(std::move(update));
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
    chunkManager_.setCoreChunks(hello.coreChunks);
    logging::info("Server assigned session {} with actor '{}'", hello.sessionId, hello.actorName);

    entt::entity entity = actorWorld_.createLocalPlayer(hello.actorName, hello.sessionId, hello.position, hello.playerMode);
    auto& registry = actorWorld_.registry();
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
    if (!chunkManager_.areCoreChunksReady()) {
        return;
    }

    netClient_->sendReliable(serializeClientReady());
    netClient_->flush();

    registerSystem(std::make_unique<InputSystem>(renderContext_, localSessionId_));
    registerSystem(std::make_unique<RenderSystem>(renderContext_, &chunkManager_, localSessionId_));

    chunkManager_.clearCoreChunks();
    state_ = State::Running;
    logging::info("World is ready");
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

    auto& registry = actorWorld_.registry();
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

void GameClient::replayEntitySnapshots() {
    MW_PROFILE_SCOPE("Client.ReplayEntitySnapshots");

    if (entitySnapshotBuffer_.empty()) {
        return;
    }

    NetEntitySnapshot snapshot = std::move(entitySnapshotBuffer_.front());
    entitySnapshotBuffer_.pop_front();
    if (snapshot.sequence <= lastAppliedEntitySnapshot_) {
        return;
    }

    applyEntitySnapshot(snapshot);
    lastAppliedEntitySnapshot_ = snapshot.sequence;
}

void GameClient::applyEntitySnapshot(const NetEntitySnapshot& snapshot) {
    MW_PROFILE_SCOPE("Client.ApplyEntitySnapshot");
    MW_PROFILE_COUNTER("Client.EntitySnapshotActors", static_cast<int64_t>(snapshot.actors.size()));

    auto& registry = actorWorld_.registry();
    std::unordered_set<std::string> entityNames;
    entityNames.reserve(snapshot.actors.size());
    for (const auto& actor : snapshot.actors) {
        entityNames.insert(actor.name);
        entt::entity entity = actorWorld_.getEntityByName(actor.name);
        if (entity == entt::null) {
            switch (actor.entityType) {
                case EntityType::Player:
                    entity = actorWorld_.createRemotePlayer(actor.name, actor.position, actor.playerMode);
                    break;
                case EntityType::Robot:
                    entity = actorWorld_.createRobot(actor.name, actor.position);
                    break;
            }
        }
        if (entity == entt::null) {
            continue;
        }
        if (!registry.all_of<SessionComponent>(entity)) {
            if (actor.entityType == EntityType::Player) {
                actorWorld_.setPlayerMode(entity, actor.playerMode);
            }
            queueRemoteActorSample(registry, entity, actor);
        }
    }

    std::vector<entt::entity> remoteActorsToDestroy;
    auto view = registry.view<NameComponent, TransformComponent>(entt::exclude<SessionComponent>);
    for (auto entity : view) {
        const auto& name = view.get<NameComponent>(entity);
        if (entityNames.count(name.name) == 0) {
            remoteActorsToDestroy.push_back(entity);
        }
    }
    for (entt::entity entity : remoteActorsToDestroy) {
        actorWorld_.destroyEntity(entity);
    }
}

void GameClient::applyChunkUpdate(NetChunkUpdate&& update) {
    MW_PROFILE_SCOPE("Client.ApplyChunkUpdate");

    bool applied = false;
    if (update.operation == NetChunkOperation::Unload) {
        applied = chunkManager_.unload(update.chunkPos, update.revision);
    } else {
        applied = chunkManager_.upsert(update.chunkPos, update.revision, std::move(update.blocks));
    }

    if (applied) {
        MW_PROFILE_COUNTER("Client.ChunkUpdatesApplied", 1);
    } else {
        MW_PROFILE_COUNTER("Client.ChunkUpdatesDropped", 1);
    }
}

void GameClient::rebuildChunkMeshes() {
    MW_PROFILE_SCOPE("Client.RebuildChunkMeshes");

    const ClientChunkManager::MeshFocus focus = localPlayerMeshFocus();
    const auto start = std::chrono::steady_clock::now();
    size_t attemptedCount = 0;
    while (attemptedCount < kMaxChunkMeshRebuildsPerFrame) {
        const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (attemptedCount > 0 && elapsedMs >= kMaxChunkMeshRebuildTimePerFrame) {
            break;
        }

        const std::optional<ClientChunkManager::MeshTask> task = chunkManager_.takeNextMeshTask(focus);
        if (!task) {
            break;
        }

        const ChunkMesh mesh = buildChunkMesh(voxelWorld_, task->chunkPos);
        const ClientChunkManager::MeshTaskResult result = chunkManager_.completeMeshTask(*task, mesh);
        ++attemptedCount;
        if (result == ClientChunkManager::MeshTaskResult::Accepted) {
            MW_PROFILE_COUNTER("Client.ChunkMeshesRebuilt", 1);
        } else if (result == ClientChunkManager::MeshTaskResult::Exhausted) {
            break;
        }
    }

    MW_PROFILE_GAUGE("Client.MeshRebuildBacklog", static_cast<double>(chunkManager_.dirtyMeshCount()));
}

ClientChunkManager::MeshFocus GameClient::localPlayerMeshFocus() const {
    const auto& registry = actorWorld_.registry();
    auto localPlayers = registry.view<SessionComponent, TransformComponent>();
    for (entt::entity entity : localPlayers) {
        if (localPlayers.get<SessionComponent>(entity).sessionId == localSessionId_) {
            const auto& transform = localPlayers.get<TransformComponent>(entity);
            return ClientChunkManager::MeshFocus{
                ChunkLayout::worldToChunk(glm::ivec3(glm::floor(transform.position))),
                Direction::lookForward(transform.rotation.y, transform.rotation.x),
            };
        }
    }
    return ClientChunkManager::MeshFocus{};
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
    auto& registry = actorWorld_.registry();
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
