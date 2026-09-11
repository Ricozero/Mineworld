#include "net_protocol.h"

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/verifier.h>

#include <type_traits>

#include "chunk_layout.h"

namespace {

constexpr size_t kMaxActors = 2048;
constexpr size_t kMaxCommandArguments = 16;
constexpr size_t kMaxCommandStringLength = 256;

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

template <typename Enum>
using EnumValue = std::underlying_type_t<Enum>;

template <typename Enum>
EnumValue<Enum> toWireEnum(Enum value) {
    static_assert(std::is_unsigned_v<EnumValue<Enum>>);
    return static_cast<EnumValue<Enum>>(value);
}

template <typename Enum>
Enum fromWireEnum(EnumValue<Enum> value, Enum fallback) {
    return value < toWireEnum(Enum::Count) ? static_cast<Enum>(value) : fallback;
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
            toWireEnum(hello.playerMode),
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
    result.playerMode = fromWireEnum(hello->player_mode(), PlayerMode::Survival);
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
            toWireEnum(input.playerMode),
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
    outInput.playerMode = fromWireEnum(input->player_mode(), PlayerMode::Survival);
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
            toWireEnum(actor.entityType),
            toWireEnum(actor.playerMode)));
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
                fromWireEnum(actor->entity_type(), EntityType::Player),
                fromWireEnum(actor->player_mode(), PlayerMode::Survival),
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
        update.blocks.serialize(blockBytes);
        blocks = builder.CreateVector(blockBytes);
    }
    const auto payload = mineworld::net::CreateChunkUpdate(
        builder,
        &chunkPos,
        update.revision,
        toWireEnum(update.operation),
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
    if (!ChunkLayout::isChunkInWorld(result.chunkPos)) {
        return false;
    }
    result.revision = update->revision();
    result.operation = fromWireEnum(update->operation(), NetChunkOperation::Upsert);
    const auto* blockBytes = update->blocks();
    if (result.operation == NetChunkOperation::Unload) {
        if (blockBytes && !blockBytes->empty()) {
            return false;
        }
    } else {
        if (!blockBytes || blockBytes->size() > ChunkData::MAX_SERIALIZED_SIZE) {
            return false;
        }
        if (!ChunkData::deserialize(std::span<const uint8_t>(blockBytes->data(), blockBytes->size()), result.blocks)) {
            return false;
        }
    }
    outUpdate = std::move(result);
    return true;
}

std::vector<uint8_t> serializeCommandRequest(const CommandRequest& command) {
    return finishMessage([&](flatbuffers::FlatBufferBuilder& builder) {
        std::vector<flatbuffers::Offset<mineworld::net::CommandArgument>> arguments;
        arguments.reserve(command.arguments.size());
        for (const CommandArgument& argument : command.arguments) {
            mineworld::net::CommandArgumentValue type = mineworld::net::CommandArgumentValue::NONE;
            flatbuffers::Offset<void> value;
            if (const auto* stringValue = std::get_if<std::string>(&argument)) {
                type = mineworld::net::CommandArgumentValue::StringArgument;
                value = mineworld::net::CreateStringArgument(builder, builder.CreateString(*stringValue)).Union();
            } else if (const auto* integerValue = std::get_if<int64_t>(&argument)) {
                type = mineworld::net::CommandArgumentValue::IntegerArgument;
                value = mineworld::net::CreateIntegerArgument(builder, *integerValue).Union();
            } else if (const auto* floatValue = std::get_if<double>(&argument)) {
                type = mineworld::net::CommandArgumentValue::FloatArgument;
                value = mineworld::net::CreateFloatArgument(builder, *floatValue).Union();
            } else if (const auto* boolValue = std::get_if<bool>(&argument)) {
                type = mineworld::net::CommandArgumentValue::BoolArgument;
                value = mineworld::net::CreateBoolArgument(builder, *boolValue).Union();
            } else if (const auto* vec3Value = std::get_if<glm::vec3>(&argument)) {
                type = mineworld::net::CommandArgumentValue::Vec3Argument;
                const mineworld::net::Vec3 wireValue = toFbVec3(*vec3Value);
                value = mineworld::net::CreateVec3Argument(builder, &wireValue).Union();
            }
            arguments.push_back(mineworld::net::CreateCommandArgument(builder, type, value));
        }
        return mineworld::net::CreateCommandRequest(
            builder,
            command.requestId,
            toWireEnum(command.operation),
            builder.CreateVector(arguments));
    });
}

bool deserializeCommandRequest(std::span<const uint8_t> bytes, CommandRequest& outCommand) {
    const mineworld::net::NetMessage* message = tryGetMessage(bytes);
    if (!message || message->payload_type() != mineworld::net::NetMessagePayload::CommandRequest) {
        return false;
    }
    const mineworld::net::CommandRequest* command = message->payload_as_CommandRequest();
    if (!command || (command->arguments() && command->arguments()->size() > kMaxCommandArguments)) {
        return false;
    }

    CommandRequest result;
    result.requestId = command->request_id();
    result.operation = fromWireEnum(command->operation(), CommandOperation::None);
    if (const auto* arguments = command->arguments()) {
        result.arguments.reserve(arguments->size());
        for (const mineworld::net::CommandArgument* argument : *arguments) {
            if (!argument) {
                return false;
            }
            switch (argument->value_type()) {
                case mineworld::net::CommandArgumentValue::StringArgument: {
                    const auto* value = argument->value_as_StringArgument();
                    if (!value || !value->value() || value->value()->size() > kMaxCommandStringLength) {
                        return false;
                    }
                    result.arguments.emplace_back(value->value()->str());
                    break;
                }
                case mineworld::net::CommandArgumentValue::IntegerArgument: {
                    const auto* value = argument->value_as_IntegerArgument();
                    if (!value) {
                        return false;
                    }
                    result.arguments.emplace_back(value->value());
                    break;
                }
                case mineworld::net::CommandArgumentValue::FloatArgument: {
                    const auto* value = argument->value_as_FloatArgument();
                    if (!value) {
                        return false;
                    }
                    result.arguments.emplace_back(value->value());
                    break;
                }
                case mineworld::net::CommandArgumentValue::BoolArgument: {
                    const auto* value = argument->value_as_BoolArgument();
                    if (!value) {
                        return false;
                    }
                    result.arguments.emplace_back(value->value());
                    break;
                }
                case mineworld::net::CommandArgumentValue::Vec3Argument: {
                    const auto* value = argument->value_as_Vec3Argument();
                    if (!value || !value->value()) {
                        return false;
                    }
                    result.arguments.emplace_back(fromFbVec3(value->value()));
                    break;
                }
                default:
                    return false;
            }
        }
    }
    outCommand = std::move(result);
    return true;
}

std::vector<uint8_t> serializeCommandResponse(const CommandResponse& response) {
    return finishMessage([&](flatbuffers::FlatBufferBuilder& builder) {
        return mineworld::net::CreateCommandResponse(
            builder,
            response.requestId,
            toWireEnum(response.status));
    });
}

bool deserializeCommandResponse(std::span<const uint8_t> bytes, CommandResponse& outResponse) {
    const mineworld::net::NetMessage* message = tryGetMessage(bytes);
    if (!message || message->payload_type() != mineworld::net::NetMessagePayload::CommandResponse) {
        return false;
    }
    const mineworld::net::CommandResponse* response = message->payload_as_CommandResponse();
    if (!response) {
        return false;
    }
    outResponse.requestId = response->request_id();
    outResponse.status = fromWireEnum(response->status(), CommandStatus::Failed);
    return true;
}
