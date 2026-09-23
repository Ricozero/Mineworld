#include "game_client.h"

#include <algorithm>
#include <chrono>

#include "chunk_mesh.h"
#include "client_system.h"
#include "entity.h"
#include "log.h"
#include "net_kcp.h"
#include "profiler.h"
#include "render_context.h"
#include "text.h"

namespace {

constexpr float kConnectionTimeoutSeconds = 10.0f;
constexpr size_t kMaxChunkMeshRebuildsPerFrame = 1024;
constexpr double kMaxChunkMeshRebuildTimePerFrame = 8.0;

}  // namespace

GameClient::GameClient(RenderContext* renderContext, std::string address, uint16_t port)
    : GameClient(renderContext, nullptr) {
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

GameClient::GameClient(RenderContext* renderContext, std::unique_ptr<INetClient> netClient)
    : chunkManager_(voxelWorld_, renderContext != nullptr), netClient_(std::move(netClient)), renderContext_(renderContext) {
}

GameClient::~GameClient() = default;

void GameClient::registerSystem(std::unique_ptr<System> system) {
    systems_.push_back(std::move(system));
}

void GameClient::update(float deltaTime) {
    MW_PROFILE_SCOPE("Client.Update");

    if (state_ == State::Failed) {
        return;
    }

    secondsSincePacket_ += deltaTime;
    pumpNetwork();
    processNetworkEvents();
    if (state_ == State::Failed) {
        return;
    }
    if (state_ == State::Disconnecting) {
        disconnect();
        return;
    }
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
    sendPendingMessages();
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
}

void GameClient::processNetworkEvents() {
    MW_PROFILE_SCOPE("Client.ProcessNetworkEvents");
    if (!netClient_) {
        return;
    }
    NetEvent event;
    while (netClient_->popEvent(event)) {
        switch (event.type) {
            case NetEventType::Connected:
                secondsSincePacket_ = 0.0f;
                break;
            case NetEventType::Packet:
                secondsSincePacket_ = 0.0f;
                if (state_ != State::Disconnecting && state_ != State::Failed) {
                    onServerPacket(event.payload);
                }
                break;
            case NetEventType::Disconnected:
                if (state_ != State::Disconnecting) {
                    fail("Transport disconnected");
                }
                break;
        }
    }
    if (state_ == State::Connecting && helloPending_ && netClient_->isConnected() && netClient_->send(serializeClientHello())) {
        helloPending_ = false;
        state_ = State::Awaiting;
        secondsSincePacket_ = 0.0f;
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
        case Payload::ChunkUpsertBatch: {
            std::vector<NetDecodedChunkUpsert> chunks;
            if (!deserializeChunkUpsertBatch(packet, chunks)) {
                fail("Invalid chunk upsert batch");
                break;
            }
            for (auto& chunk : chunks) {
                const bool applied = chunkManager_.upsert(chunk.chunkPos, chunk.revision, std::move(chunk.blocks));
                MW_PROFILE_COUNTER("Client.ChunkUpsertApplied", applied ? 1 : 0);
                MW_PROFILE_COUNTER("Client.ChunkUpsertDropped", applied ? 0 : 1);
            }
            break;
        }
        case Payload::ChunkUnloadBatch: {
            std::vector<NetChunkUnload> chunks;
            if (!deserializeChunkUnloadBatch(packet, chunks)) {
                fail("Invalid chunk unload batch");
                break;
            }
            for (const auto& chunk : chunks) {
                const bool applied = chunkManager_.unload(chunk.chunkPos, chunk.revision);
                MW_PROFILE_COUNTER("Client.ChunkUnloadApplied", applied ? 1 : 0);
                MW_PROFILE_COUNTER("Client.ChunkUnloadDropped", applied ? 0 : 1);
            }
            break;
        }
        case Payload::CommandResponse: {
            CommandResponse response;
            if (deserializeCommandResponse(packet, response)) {
                if (response.requestId == 0 || outstandingCommands_.erase(response.requestId) != 0) pushMessage(std::move(response));
            }
            break;
        }
        case Payload::ChatMessage: {
            ChatMessage message;
            if (deserializeChatMessage(packet, message)) pushMessage(std::move(message));
            break;
        }
        default:
            logging::warn("Ignored unknown server packet");
            break;
    }
}

void GameClient::disconnect() {
    if (!netClient_ || state_ == State::Failed) {
        return;
    }
    state_ = State::Disconnecting;
    pendingMessagePackets_.clear();
    if (!outstandingCommands_.empty()) pushMessage(CommandResponse{0, false, "Disconnected before all command results arrived."});
    outstandingCommands_.clear();
    if (helloPending_) {
        netClient_->close();
    } else if (!disconnectSent_ && netClient_->send(serializeClientDisconnect())) {
        disconnectSent_ = true;
        logging::info("Requested disconnect from server (session {})", localSessionId_);
    }
    netClient_->flush();
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

    entt::entity entity = actorWorld_.createLocalPlayer(hello.actorId, hello.actorName, hello.sessionId, hello.position, hello.playerMode);
    auto& registry = actorWorld_.registry();
    if (!registry.valid(entity) || !registry.all_of<TransformComponent>(entity)) {
        logging::error("Failed to create player {} '{}' for session {}", hello.actorId, hello.actorName, hello.sessionId);
        fail("Failed to create local player");
        return;
    }
    auto& transform = registry.get<TransformComponent>(entity);
    transform.rotation.y = hello.yaw;
    transform.rotation.x = hello.pitch;

    logging::info("Created player {} '{}' for session {}", hello.actorId, hello.actorName, hello.sessionId);
    state_ = State::Loading;
}

void GameClient::tryEnterRunning() {
    if (!chunkManager_.areCoreChunksReady()) {
        return;
    }

    if (!netClient_->send(serializeClientReady())) {
        return;
    }
    netClient_->flush();

    if (renderContext_) {
        registerSystem(std::make_unique<InputSystem>(renderContext_, localSessionId_));
        registerSystem(std::make_unique<RenderSystem>(renderContext_, chunkManager_, chunkCuller_, localSessionId_));
    }

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
    pendingMessagePackets_.clear();
    if (!outstandingCommands_.empty()) pushMessage(CommandResponse{0, false, "Disconnected before all command results arrived."});
    outstandingCommands_.clear();
    if (netClient_) {
        netClient_->close();
    }
    if (localSessionId_ != 0) {
        logging::warn("Disconnected from server (session {}): {}", localSessionId_, failureReason_);
    } else {
        logging::warn("Connection failed: {}", failureReason_);
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
        netClient_->send(serializeClientInput(input));
        break;
    }
}

void GameClient::pushMessage(Message message) {
    constexpr size_t kMaxMessages = 256;
    if (messages_.size() == kMaxMessages) messages_.pop_front();
    messages_.push_back(std::move(message));
}

std::optional<GameClient::Message> GameClient::consumeMessage() {
    if (messages_.empty()) return std::nullopt;
    Message message = std::move(messages_.front());
    messages_.pop_front();
    return message;
}

void GameClient::submitText(std::string_view text) {
    text = trimText(text);
    if (text.empty()) return;
    if (text.starts_with('/')) submitCommand(text);
    else submitChat(text);
}

void GameClient::submitChat(std::string_view text) {
    text = trimText(text);
    if (text.empty()) return;
    if (state_ != State::Running) {
        pushMessage(CommandResponse{0, false, "Session is not ready."});
        return;
    }
    auto packet = serializeChatRequest(text);
    if (packet.empty()) {
        pushMessage(CommandResponse{0, false, "Chat must be single-line UTF-8, at most 1024 bytes."});
    } else if (pendingMessagePackets_.size() >= 64) {
        pushMessage(CommandResponse{0, false, "Too many queued messages. Try again later."});
    } else {
        pendingMessagePackets_.push_back(std::move(packet));
    }
}

void GameClient::submitCommand(std::string_view text) {
    text = trimText(text);
    if (text.empty()) return;
    auto parsed = parseCommandLine(text);
    if (!parsed.command) {
        pushMessage(CommandResponse{0, false, std::move(parsed.error)});
        return;
    }
    std::string error;
    if (state_ != State::Running) error = "Session is not ready.";
    if (error.empty() && (pendingMessagePackets_.size() >= 64 || outstandingCommands_.size() >= 64)) error = "Too many pending commands. Try again later.";
    if (!error.empty()) {
        pushMessage(CommandResponse{0, false, std::move(error)});
        return;
    }
    auto& command = *parsed.command;
    command.requestId = nextCommandRequestId_++;
    outstandingCommands_.insert(command.requestId);
    pendingMessagePackets_.push_back(serializeCommandRequest(command));
}

void GameClient::sendPendingMessages() {
    if (!netClient_ || state_ != State::Running) return;
    size_t remaining = 16;
    while (!pendingMessagePackets_.empty() && remaining-- > 0) {
        if (!netClient_->send(pendingMessagePackets_.front())) break;
        pendingMessagePackets_.pop_front();
    }
}

void GameClient::replayEntitySnapshots() {
    MW_PROFILE_SCOPE("Client.ReplayEntitySnapshots");

    if (entitySnapshotBuffer_.empty()) {
        return;
    }

    NetEntitySnapshot snapshot = std::move(entitySnapshotBuffer_.front());
    entitySnapshotBuffer_.pop_front();
    if (snapshot.sequence <= lastEntitySnapshotSequence_) {
        return;
    }
    MW_PROFILE_COUNTER("Client.EntitySnapshotActors", static_cast<int64_t>(snapshot.actors.size()));
    auto& registry = actorWorld_.registry();
    for (const auto& actor : snapshot.actors) {
        auto entity = actorWorld_.getEntity(actor.id);
        if (entity == entt::null) {
            switch (actor.entityType) {
                case EntityType::Actor:
                    entity = actorWorld_.createActor(actor.id, actor.position);
                    break;
                case EntityType::Player:
                    entity = actorWorld_.createRemotePlayer(actor.id, actor.name, actor.position, actor.playerMode);
                    break;
                case EntityType::Robot:
                    entity = actorWorld_.createRobot(actor.id, actor.name, actor.position);
                    break;
                default:
                    continue;
            }
        }
        if (entity == entt::null) {
            continue;
        }
        actorWorld_.setName(entity, actor.name);
        if (registry.all_of<SessionComponent>(entity)) {
            continue;
        }
        registry.emplace_or_replace<ReplicationStateComponent>(entity, snapshot.sequence);
        if (actor.entityType == EntityType::Player) {
            actorWorld_.setPlayerMode(entity, actor.playerMode);
        }
        auto& interpolation = registry.get_or_emplace<InterpolationComponent>(entity);
        interpolation.samples.push_back(InterpolationSample{actor.position, glm::vec3(actor.pitch, actor.yaw, 0.0f), actor.velocity, actor.playerMode, snapshotClock_});
        constexpr size_t maxSamples = 8;
        while (interpolation.samples.size() > maxSamples) {
            interpolation.samples.pop_front();
        }
    }
    const auto view = registry.view<ReplicationStateComponent>(entt::exclude<SessionComponent>);
    for (const auto entity : view) {
        if (view.get<ReplicationStateComponent>(entity).lastSnapshotSequence != snapshot.sequence) {
            actorWorld_.destroyEntity(entity);
        }
    }
    lastEntitySnapshotSequence_ = snapshot.sequence;
}

void GameClient::rebuildChunkMeshes() {
    if (!renderContext_) {
        return;
    }
    MW_PROFILE_SCOPE("Client.RebuildChunkMeshes");

    const auto start = std::chrono::steady_clock::now();
    size_t attemptedCount = 0;
    bool exhausted = false;
    while (attemptedCount < kMaxChunkMeshRebuildsPerFrame) {
        const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (attemptedCount > 0 && elapsedMs >= kMaxChunkMeshRebuildTimePerFrame) {
            break;
        }

        const std::optional<ClientChunkManager::MeshTask> task = chunkManager_.takeNextMeshTask();
        if (!task) {
            break;
        }

        buildChunkMesh(voxelWorld_, task->chunkPos, meshScratch_);
        const ClientChunkManager::MeshTaskResult result = chunkManager_.completeMeshTask(*task, meshScratch_);
        ++attemptedCount;
        if (result == ClientChunkManager::MeshTaskResult::Accepted) {
            MW_PROFILE_COUNTER("Client.ChunkMeshesRebuilt", 1);
        } else if (result == ClientChunkManager::MeshTaskResult::Exhausted) {
            if (!meshPoolExhausted_) {
                logging::warn("Chunk mesh pool exhausted on chunk ({}, {}, {}) needing {} vertices: {:.1f} MiB reserved, {:.1f} MiB committed, {} meshes, {} chunks still dirty",
                              task->chunkPos.x, task->chunkPos.y, task->chunkPos.z, meshScratch_.vertices.size(),
                              static_cast<double>(chunkManager_.meshBytesReserved()) / (1024.0 * 1024.0),
                              static_cast<double>(chunkManager_.meshBytesCommitted()) / (1024.0 * 1024.0),
                              chunkManager_.meshCount(), chunkManager_.dirtyMeshCount());
            }
            exhausted = true;
            break;
        }
    }

    if (meshPoolExhausted_ && !exhausted) {
        logging::info("Chunk mesh pool recovered: {:.1f} MiB reserved, {} chunks still dirty",
                      static_cast<double>(chunkManager_.meshBytesReserved()) / (1024.0 * 1024.0), chunkManager_.dirtyMeshCount());
    }
    meshPoolExhausted_ = exhausted;

    MW_PROFILE_GAUGE("Client.MeshRebuildBacklog", static_cast<double>(chunkManager_.dirtyMeshCount()));
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
