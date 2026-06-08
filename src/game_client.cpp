#include "game_client.h"

#include <algorithm>

#include "entity.h"
#include "log.h"
#include "net_kcp.h"
#include "profiler.h"
#include "render_context.h"
#include "system.h"

GameClient::GameClient(RenderContext* renderContext, std::string address, uint16_t port)
    : renderContext_(renderContext) {
    logging::Scope logScope(logging::Channel::Client);

    auto channel = std::make_unique<KcpChannel>(ioContext_, 0);
    asio::error_code addressError;
    asio::ip::address resolvedAddress = asio::ip::make_address(address, addressError);
    if (addressError) {
        logging::warn("Invalid server address '{}': {}", address, addressError.message());
        resolvedAddress = asio::ip::make_address("127.0.0.1");
    }
    const auto serverEndpoint = IPacketChannel::Endpoint(
        resolvedAddress,
        port);
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
    updateRemoteInterpolation(deltaTime);
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

void GameClient::disconnect() {
    if (disconnectSent_ || !channel_ || helloPending_) {
        return;
    }

    channel_->sendReliable(serializeClientDisconnect());
    channel_->flush();
    disconnectSent_ = true;
}

void GameClient::handleServerHello(const NetServerHello& hello) {
    if (sessionReady_) {
        logging::warn("Received duplicate ServerHello, ignoring");
        return;
    }

    localSessionId_ = hello.sessionId;
    logging::info("Server assigned session {} with actor '{}'", hello.sessionId, hello.actorName);

    // Create the local player based on server instructions. Spectator is a player mode.
    entt::entity entity = world_.createLocalPlayer(hello.actorName, hello.sessionId, hello.position, hello.playerMode);
    if (entity != entt::null) {
        auto& registry = world_.getActorWorld().registry();
        auto& transform = registry.get<TransformComponent>(entity);
        transform.rotation.y = hello.yaw;
        transform.rotation.x = hello.pitch;
        registry.emplace_or_replace<PredictedInputComponent>(entity);
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
    if (!inputSystem_ || !inputSystem_->hasPendingInput()) {
        return;
    }

    auto& registry = world_.getActorWorld().registry();
    auto view = registry.view<SessionComponent, TransformComponent, ControllerInputComponent>();
    for (auto entity : view) {
        const auto& session = registry.get<SessionComponent>(entity);
        if (session.sessionId != localSessionId_) {
            continue;
        }
        const auto& transform = registry.get<TransformComponent>(entity);
        const auto& controllerInput = registry.get<ControllerInputComponent>(entity);
        NetClientInput input;
        input.move = controllerInput.move;
        input.yaw = transform.rotation.y;
        input.pitch = transform.rotation.x;
        input.jump = controllerInput.jump;
        input.sprint = controllerInput.sprint;
        input.deltaTime = controllerInput.deltaTime;
        if (registry.all_of<PlayerComponent>(entity)) {
            auto& player = registry.get<PlayerComponent>(entity);
            input.playerMode = player.mode;
            player.jumpRequested = false;
        }
        if (registry.all_of<PredictedInputComponent>(entity)) {
            auto& prediction = registry.get<PredictedInputComponent>(entity);
            input.sequence = prediction.nextInputSequence++;
            ControllerInputComponent pendingInput = controllerInput;
            pendingInput.sequence = input.sequence;
            prediction.pendingInputs.push_back(PredictedInput{pendingInput, input.playerMode, transform.rotation, controllerInput.deltaTime});
        }
        channel_->sendReliable(serializeClientInput(input));
        break;
    }

    inputSystem_->clearPendingInput();
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
        if (registry.all_of<SessionComponent>(entity)) {
            const auto& session = registry.get<SessionComponent>(entity);
            if (session.sessionId == localSessionId_) {
                reconcileLocalActor(registry, entity, actor);
                continue;
            }
        }
        if (actor.isPlayer) {
            world_.getActorWorld().setPlayerMode(entity, actor.playerMode);
        }
        queueRemoteActorSample(registry, entity, actor);
    }
}

void GameClient::reconcileLocalActor(entt::registry& registry, entt::entity entity, const NetActorState& actor) {
    auto& transform = registry.get<TransformComponent>(entity);
    const glm::vec3 currentRotation = transform.rotation;
    transform.position = actor.position;
    transform.rotation.y = actor.yaw;
    transform.rotation.x = actor.pitch;

    if (registry.all_of<PhysicsComponent>(entity)) {
        auto& physics = registry.get<PhysicsComponent>(entity);
        physics.velocity = actor.velocity;
        // isGrounded is resolved naturally by moveWithCollision during replay.
    }
    if (registry.all_of<PlayerComponent>(entity)) {
        registry.get<PlayerComponent>(entity).mode = actor.playerMode;
    }
    if (registry.all_of<PredictedInputComponent>(entity)) {
        auto& prediction = registry.get<PredictedInputComponent>(entity);
        prediction.lastAcknowledgedInputSequence = std::max(prediction.lastAcknowledgedInputSequence, actor.lastInputSequence);
        while (!prediction.pendingInputs.empty() &&
               prediction.pendingInputs.front().input.sequence <= prediction.lastAcknowledgedInputSequence) {
            prediction.pendingInputs.pop_front();
        }
        if (registry.all_of<ControllerInputComponent, PlayerComponent>(entity)) {
            const ControllerInputComponent currentInput = registry.get<ControllerInputComponent>(entity);
            for (const auto& pending : prediction.pendingInputs) {
                applyClientPredictedInput(world_, registry, entity, pending);
            }
            registry.get<ControllerInputComponent>(entity) = currentInput;
        }
    }
    transform.rotation = currentRotation;
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
    snapshotClock_ += deltaTime;

    constexpr double interpolationDelay = 0.10;
    const double renderTime = snapshotClock_ - interpolationDelay;
    auto& registry = world_.getActorWorld().registry();
    auto view = registry.view<TransformComponent, InterpolationComponent>();
    for (auto entity : view) {
        if (registry.all_of<SessionComponent>(entity)) {
            continue;
        }

        auto& interpolation = registry.get<InterpolationComponent>(entity);
        auto& samples = interpolation.samples;
        if (samples.empty()) {
            continue;
        }

        while (samples.size() >= 2 && samples[1].time <= renderTime) {
            samples.pop_front();
        }

        auto& transform = registry.get<TransformComponent>(entity);
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
