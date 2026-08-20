#include "net_protocol.h"

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/verifier.h>

#include "chunk.h"

namespace {

constexpr size_t kMaxActors = 2048;

template <typename Payload>
std::vector<uint8_t> finishMessage(flatbuffers::FlatBufferBuilder& builder, flatbuffers::Offset<Payload> payload) {
    constexpr auto payloadType = mineworld::net::NetMessagePayloadTraits<Payload>::enum_value;
    static_assert(payloadType != mineworld::net::NetMessagePayload::NONE);
    const auto message = mineworld::net::CreateNetMessage(builder, payloadType, payload.Union());
    mineworld::net::FinishNetMessageBuffer(builder, message);
    const uint8_t* data = builder.GetBufferPointer();
    return std::vector<uint8_t>(data, data + builder.GetSize());
}

template <typename PayloadBuilder>
std::vector<uint8_t> finishMessage(PayloadBuilder payloadBuilder) {
    flatbuffers::FlatBufferBuilder builder;
    return finishMessage(builder, payloadBuilder(builder));
}

const mineworld::net::NetMessage* tryGetMessage(std::span<const uint8_t> bytes) {
    flatbuffers::Verifier verifier(bytes.data(), bytes.size());
    if (!mineworld::net::VerifyNetMessageBuffer(verifier)) {
        return nullptr;
    }
    return mineworld::net::GetNetMessage(bytes.data());
}

mineworld::net::Vec3 toFbVec3(const glm::vec3& value) {
    return mineworld::net::Vec3(value.x, value.y, value.z);
}

mineworld::net::IVec3 toFbIVec3(const glm::ivec3& value) {
    return mineworld::net::IVec3(value.x, value.y, value.z);
}

glm::vec3 fromFbVec3(const mineworld::net::Vec3* value) {
    return value ? glm::vec3(value->x(), value->y(), value->z()) : glm::vec3(0.0f);
}

glm::ivec3 fromFbIVec3(const mineworld::net::IVec3* value) {
    return value ? glm::ivec3(value->x(), value->y(), value->z()) : glm::ivec3(0);
}

uint8_t toWireEntityType(EntityType type) {
    return static_cast<uint8_t>(type);
}

EntityType fromWireEntityType(uint8_t type) {
    return type == static_cast<uint8_t>(EntityType::Robot) ? EntityType::Robot : EntityType::Player;
}

uint8_t toWirePlayerMode(PlayerMode mode) {
    return static_cast<uint8_t>(mode);
}

PlayerMode fromWirePlayerMode(uint8_t mode) {
    return mode == static_cast<uint8_t>(PlayerMode::Spectator) ? PlayerMode::Spectator : PlayerMode::Survival;
}

mineworld::net::ChunkOperation toWireChunkOperation(NetChunkOperation operation) {
    return operation == NetChunkOperation::Unload ? mineworld::net::ChunkOperation::Unload : mineworld::net::ChunkOperation::Upsert;
}

NetChunkOperation fromWireChunkOperation(mineworld::net::ChunkOperation operation) {
    return operation == mineworld::net::ChunkOperation::Unload ? NetChunkOperation::Unload : NetChunkOperation::Upsert;
}

bool isValidBlock(BlockType type, BlockOrientation orientation) {
    return static_cast<uint8_t>(type) <= static_cast<uint8_t>(BlockType::Sand) &&
           static_cast<uint8_t>(orientation) <= static_cast<uint8_t>(BlockOrientation::Down);
}

}  // namespace

mineworld::net::NetMessagePayload getPacketType(std::span<const uint8_t> bytes) {
    const mineworld::net::NetMessage* message = tryGetMessage(bytes);
    return message ? message->payload_type() : mineworld::net::NetMessagePayload::NONE;
}

std::vector<uint8_t> serializeClientHello() {
    return finishMessage([](flatbuffers::FlatBufferBuilder& builder) {
        return mineworld::net::CreateClientHello(builder);
    });
}

std::vector<uint8_t> serializeClientDisconnect() {
    return finishMessage([](flatbuffers::FlatBufferBuilder& builder) {
        return mineworld::net::CreateClientDisconnect(builder);
    });
}

std::vector<uint8_t> serializeClientReady() {
    return finishMessage([](flatbuffers::FlatBufferBuilder& builder) {
        return mineworld::net::CreateClientReady(builder);
    });
}

std::vector<uint8_t> serializeServerHello(const NetServerHello& hello) {
    return finishMessage([&](flatbuffers::FlatBufferBuilder& builder) {
        const auto name = builder.CreateString(hello.actorName);
        const mineworld::net::Vec3 position = toFbVec3(hello.position);
        std::vector<mineworld::net::IVec3> coreChunks;
        coreChunks.reserve(hello.coreChunks.size());
        for (const glm::ivec3& chunkPos : hello.coreChunks) {
            coreChunks.push_back(toFbIVec3(chunkPos));
        }
        return mineworld::net::CreateServerHello(
            builder,
            hello.sessionId,
            name,
            &position,
            hello.yaw,
            hello.pitch,
            toWirePlayerMode(hello.playerMode),
            builder.CreateVectorOfStructs(coreChunks));
    });
}

bool deserializeServerHello(std::span<const uint8_t> bytes, NetServerHello& outHello) {
    const mineworld::net::NetMessage* message = tryGetMessage(bytes);
    if (!message || message->payload_type() != mineworld::net::NetMessagePayload::ServerHello) {
        return false;
    }
    const mineworld::net::ServerHello* hello = message->payload_as_ServerHello();
    if (!hello || !hello->actor_name()) {
        return false;
    }

    NetServerHello result;
    result.sessionId = hello->session_id();
    result.actorName = hello->actor_name()->str();
    result.position = fromFbVec3(hello->position());
    result.yaw = hello->yaw();
    result.pitch = hello->pitch();
    result.playerMode = fromWirePlayerMode(hello->player_mode());
    if (const auto* coreChunks = hello->core_chunks()) {
        result.coreChunks.reserve(coreChunks->size());
        for (const mineworld::net::IVec3* chunkPos : *coreChunks) {
            result.coreChunks.push_back(fromFbIVec3(chunkPos));
        }
    }
    outHello = std::move(result);
    return true;
}

std::vector<uint8_t> serializeClientInput(const NetClientInput& input) {
    return finishMessage([&](flatbuffers::FlatBufferBuilder& builder) {
        const mineworld::net::Vec3 position = toFbVec3(input.position);
        const mineworld::net::Vec3 velocity = toFbVec3(input.velocity);
        return mineworld::net::CreateClientInput(
            builder,
            &position,
            &velocity,
            input.yaw,
            input.pitch,
            toWirePlayerMode(input.playerMode),
            input.sequence);
    });
}

bool deserializeClientInput(std::span<const uint8_t> bytes, NetClientInput& outInput) {
    const mineworld::net::NetMessage* message = tryGetMessage(bytes);
    if (!message || message->payload_type() != mineworld::net::NetMessagePayload::ClientInput) {
        return false;
    }
    const mineworld::net::ClientInput* input = message->payload_as_ClientInput();
    if (!input) {
        return false;
    }
    outInput.position = fromFbVec3(input->position());
    outInput.velocity = fromFbVec3(input->velocity());
    outInput.yaw = input->yaw();
    outInput.pitch = input->pitch();
    outInput.playerMode = fromWirePlayerMode(input->player_mode());
    outInput.sequence = input->sequence();
    return true;
}

std::vector<uint8_t> serializeEntitySnapshot(const NetEntitySnapshot& snapshot, flatbuffers::FlatBufferBuilder& builder) {
    builder.Reset();
    std::vector<flatbuffers::Offset<mineworld::net::ActorState>> actors;
    actors.reserve(snapshot.actors.size());
    for (const NetActorState& actor : snapshot.actors) {
        const auto name = builder.CreateString(actor.name);
        const mineworld::net::Vec3 position = toFbVec3(actor.position);
        const mineworld::net::Vec3 velocity = toFbVec3(actor.velocity);
        actors.push_back(mineworld::net::CreateActorState(
            builder,
            name,
            &position,
            &velocity,
            actor.yaw,
            actor.pitch,
            toWireEntityType(actor.entityType),
            toWirePlayerMode(actor.playerMode)));
    }
    const auto payload = mineworld::net::CreateEntitySnapshot(builder, snapshot.sequence, builder.CreateVector(actors));
    return finishMessage(builder, payload);
}

bool deserializeEntitySnapshot(std::span<const uint8_t> bytes, NetEntitySnapshot& outSnapshot) {
    const mineworld::net::NetMessage* message = tryGetMessage(bytes);
    if (!message || message->payload_type() != mineworld::net::NetMessagePayload::EntitySnapshot) {
        return false;
    }
    const mineworld::net::EntitySnapshot* snapshot = message->payload_as_EntitySnapshot();
    if (!snapshot) {
        return false;
    }

    NetEntitySnapshot result;
    result.sequence = snapshot->sequence();
    if (const auto* actors = snapshot->actors()) {
        if (actors->size() > kMaxActors) {
            return false;
        }
        result.actors.reserve(actors->size());
        for (const mineworld::net::ActorState* actor : *actors) {
            if (!actor || !actor->name()) {
                return false;
            }
            result.actors.push_back(NetActorState{
                actor->name()->str(),
                fromFbVec3(actor->position()),
                fromFbVec3(actor->velocity()),
                actor->yaw(),
                actor->pitch(),
                fromWireEntityType(actor->entity_type()),
                fromWirePlayerMode(actor->player_mode()),
            });
        }
    }
    outSnapshot = std::move(result);
    return true;
}

std::vector<uint8_t> serializeChunkUpdate(const NetChunkUpdate& update, flatbuffers::FlatBufferBuilder& builder) {
    builder.Reset();
    const mineworld::net::IVec3 chunkPos = toFbIVec3(update.chunkPos);
    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> blocks;
    if (update.operation == NetChunkOperation::Upsert) {
        std::vector<uint8_t> blockBytes;
        blockBytes.reserve(update.blocks.size() * NetChunkUpdate::SERIALIZED_BLOCK_SIZE);
        for (const BlockData& block : update.blocks) {
            blockBytes.push_back(static_cast<uint8_t>(block.type));
            blockBytes.push_back(static_cast<uint8_t>(block.orientation));
        }
        blocks = builder.CreateVector(blockBytes);
    }
    const auto payload = mineworld::net::CreateChunkUpdate(
        builder,
        &chunkPos,
        update.revision,
        toWireChunkOperation(update.operation),
        blocks);
    return finishMessage(builder, payload);
}

bool deserializeChunkUpdate(std::span<const uint8_t> bytes, NetChunkUpdate& outUpdate) {
    const mineworld::net::NetMessage* message = tryGetMessage(bytes);
    if (!message || message->payload_type() != mineworld::net::NetMessagePayload::ChunkUpdate) {
        return false;
    }
    const mineworld::net::ChunkUpdate* update = message->payload_as_ChunkUpdate();
    if (!update || !update->chunk_pos()) {
        return false;
    }

    NetChunkUpdate result;
    result.chunkPos = fromFbIVec3(update->chunk_pos());
    result.revision = update->revision();
    result.operation = fromWireChunkOperation(update->operation());
    const auto* blockBytes = update->blocks();
    if (result.operation == NetChunkOperation::Unload) {
        if (blockBytes && !blockBytes->empty()) {
            return false;
        }
    } else {
        if (!blockBytes || blockBytes->size() != ChunkData::BLOCK_COUNT * NetChunkUpdate::SERIALIZED_BLOCK_SIZE) {
            return false;
        }
        result.blocks.reserve(ChunkData::BLOCK_COUNT);
        for (size_t i = 0; i < ChunkData::BLOCK_COUNT; ++i) {
            const size_t blockOffset = i * NetChunkUpdate::SERIALIZED_BLOCK_SIZE;
            const BlockType type = static_cast<BlockType>((*blockBytes)[static_cast<flatbuffers::uoffset_t>(blockOffset + NetChunkUpdate::BLOCK_TYPE_OFFSET)]);
            const BlockOrientation orientation = static_cast<BlockOrientation>((*blockBytes)[static_cast<flatbuffers::uoffset_t>(blockOffset + NetChunkUpdate::BLOCK_ORIENTATION_OFFSET)]);
            if (!isValidBlock(type, orientation)) {
                return false;
            }
            result.blocks.emplace_back(type, orientation);
        }
    }
    outUpdate = std::move(result);
    return true;
}
