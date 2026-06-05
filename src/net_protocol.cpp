#include "net_protocol.h"

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/verifier.h>

#include <cstring>

#include "net_protocol_generated.h"

namespace {

constexpr size_t kMaxActors = 2048;
constexpr size_t kMaxChunks = 65536;
constexpr size_t kMaxBlocks = 65536;

flatbuffers::DetachedBuffer finish(flatbuffers::FlatBufferBuilder& builder,
                                   flatbuffers::Offset<mineworld::net::NetMessage> message) {
    mineworld::net::FinishNetMessageBuffer(builder, message);
    return builder.Release();
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
    if (!value) {
        return glm::vec3(0.0f);
    }
    return glm::vec3(value->x(), value->y(), value->z());
}

glm::ivec3 fromFbIVec3(const mineworld::net::IVec3* value) {
    if (!value) {
        return glm::ivec3(0);
    }
    return glm::ivec3(value->x(), value->y(), value->z());
}

uint8_t toWirePlayerMode(PlayerMode mode) {
    return static_cast<uint8_t>(mode);
}

PlayerMode fromWirePlayerMode(uint8_t mode) {
    return mode == static_cast<uint8_t>(PlayerMode::Spectator) ? PlayerMode::Spectator : PlayerMode::Survival;
}

}  // namespace

std::vector<uint8_t> serializeClientHello() {
    flatbuffers::FlatBufferBuilder builder;
    const auto hello = mineworld::net::CreateClientHello(builder);
    const auto msg = mineworld::net::CreateNetMessage(
        builder,
        mineworld::net::NetMessagePayload::ClientHello,
        hello.Union());

    flatbuffers::DetachedBuffer buffer = finish(builder, msg);
    return std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size());
}

bool deserializeClientHello(std::span<const uint8_t> bytes) {
    const mineworld::net::NetMessage* msg = tryGetMessage(bytes);
    if (!msg) {
        return false;
    }
    return msg->payload_type() == mineworld::net::NetMessagePayload::ClientHello;
}

std::vector<uint8_t> serializeClientDisconnect() {
    flatbuffers::FlatBufferBuilder builder;
    const auto disconnect = mineworld::net::CreateClientDisconnect(builder);
    const auto msg = mineworld::net::CreateNetMessage(
        builder,
        mineworld::net::NetMessagePayload::ClientDisconnect,
        disconnect.Union());

    flatbuffers::DetachedBuffer buffer = finish(builder, msg);
    return std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size());
}

bool deserializeClientDisconnect(std::span<const uint8_t> bytes) {
    const mineworld::net::NetMessage* msg = tryGetMessage(bytes);
    if (!msg) {
        return false;
    }
    return msg->payload_type() == mineworld::net::NetMessagePayload::ClientDisconnect;
}

std::vector<uint8_t> serializeServerHello(const NetServerHello& hello) {
    flatbuffers::FlatBufferBuilder builder;
    const auto nameOffset = builder.CreateString(hello.actorName);
    const mineworld::net::Vec3 position = toFbVec3(hello.position);
    const auto helloOffset = mineworld::net::CreateServerHello(
        builder,
        hello.sessionId,
        nameOffset,
        &position,
        hello.yaw,
        hello.pitch,
        toWirePlayerMode(hello.playerMode));
    const auto msg = mineworld::net::CreateNetMessage(
        builder,
        mineworld::net::NetMessagePayload::ServerHello,
        helloOffset.Union());

    flatbuffers::DetachedBuffer buffer = finish(builder, msg);
    return std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size());
}

bool deserializeServerHello(std::span<const uint8_t> bytes, NetServerHello& outHello) {
    const mineworld::net::NetMessage* msg = tryGetMessage(bytes);
    if (!msg || msg->payload_type() != mineworld::net::NetMessagePayload::ServerHello) {
        return false;
    }

    const mineworld::net::ServerHello* fbHello = msg->payload_as_ServerHello();
    if (!fbHello) {
        return false;
    }

    outHello.sessionId = fbHello->session_id();
    outHello.actorName = fbHello->actor_name() ? fbHello->actor_name()->str() : "";
    outHello.position = fromFbVec3(fbHello->position());
    outHello.yaw = fbHello->yaw();
    outHello.pitch = fbHello->pitch();
    outHello.playerMode = fromWirePlayerMode(fbHello->player_mode());
    return true;
}

std::vector<uint8_t> serializeClientInput(const NetClientInput& input) {
    flatbuffers::FlatBufferBuilder builder;
    const mineworld::net::Vec3 move = toFbVec3(input.move);
    const auto inputOffset = mineworld::net::CreateClientInput(
        builder,
        &move,
        input.yaw,
        input.pitch,
        toWirePlayerMode(input.playerMode),
        input.jump,
        input.sprint,
        input.sequence);
    const auto msg = mineworld::net::CreateNetMessage(
        builder,
        mineworld::net::NetMessagePayload::ClientInput,
        inputOffset.Union());

    flatbuffers::DetachedBuffer buffer = finish(builder, msg);
    return std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size());
}

bool deserializeClientInput(std::span<const uint8_t> bytes, NetClientInput& outInput) {
    const mineworld::net::NetMessage* msg = tryGetMessage(bytes);
    if (!msg || msg->payload_type() != mineworld::net::NetMessagePayload::ClientInput) {
        return false;
    }

    const mineworld::net::ClientInput* fbInput = msg->payload_as_ClientInput();
    if (!fbInput) {
        return false;
    }

    outInput.move = fromFbVec3(fbInput->move());
    outInput.yaw = fbInput->yaw();
    outInput.pitch = fbInput->pitch();
    outInput.playerMode = fromWirePlayerMode(fbInput->player_mode());
    outInput.jump = fbInput->jump();
    outInput.sprint = fbInput->sprint();
    outInput.sequence = fbInput->sequence();
    return true;
}

std::vector<uint8_t> serializeSnapshot(const NetSnapshot& snapshot, flatbuffers::FlatBufferBuilder& builder) {
    builder.Reset();

    std::vector<flatbuffers::Offset<mineworld::net::ActorState>> actorOffsets;
    actorOffsets.reserve(snapshot.actors.size());
    for (const auto& actor : snapshot.actors) {
        const auto name = builder.CreateString(actor.name);
        const mineworld::net::Vec3 position = toFbVec3(actor.position);
        const mineworld::net::Vec3 velocity = toFbVec3(actor.velocity);
        actorOffsets.push_back(mineworld::net::CreateActorState(
            builder,
            name,
            &position,
            &velocity,
            actor.yaw,
            actor.pitch,
            actor.isPlayer,
            toWirePlayerMode(actor.playerMode),
            actor.lastInputSequence));
    }
    const auto actorsVec = builder.CreateVector(actorOffsets);

    std::vector<flatbuffers::Offset<mineworld::net::ChunkState>> chunkOffsets;
    chunkOffsets.reserve(snapshot.chunks.size());
    for (const auto& chunk : snapshot.chunks) {
        const mineworld::net::IVec3 chunkPos = toFbIVec3(chunk.chunkPos);
        chunkOffsets.push_back(mineworld::net::CreateChunkState(
            builder,
            &chunkPos,
            chunk.loaded));
    }
    const auto chunksVec = builder.CreateVector(chunkOffsets);

    std::vector<flatbuffers::Offset<mineworld::net::BlockState>> blockOffsets;
    blockOffsets.reserve(snapshot.blocks.size());
    for (const auto& block : snapshot.blocks) {
        const mineworld::net::IVec3 worldPos = toFbIVec3(block.worldPos);
        blockOffsets.push_back(mineworld::net::CreateBlockState(
            builder,
            &worldPos,
            static_cast<uint8_t>(block.data.type),
            static_cast<uint8_t>(block.data.orientation)));
    }
    const auto blocksVec = builder.CreateVector(blockOffsets);

    const auto snapshotOffset = mineworld::net::CreateSnapshot(
        builder,
        snapshot.sequence,
        actorsVec,
        chunksVec,
        blocksVec);

    const auto msg = mineworld::net::CreateNetMessage(
        builder,
        mineworld::net::NetMessagePayload::Snapshot,
        snapshotOffset.Union());

    mineworld::net::FinishNetMessageBuffer(builder, msg);
    const uint8_t* bufPtr = builder.GetBufferPointer();
    const size_t bufSize = builder.GetSize();
    return std::vector<uint8_t>(bufPtr, bufPtr + bufSize);
}

bool deserializeSnapshot(std::span<const uint8_t> bytes, NetSnapshot& outSnapshot) {
    const mineworld::net::NetMessage* msg = tryGetMessage(bytes);
    if (!msg || msg->payload_type() != mineworld::net::NetMessagePayload::Snapshot) {
        return false;
    }

    const mineworld::net::Snapshot* fbSnapshot = msg->payload_as_Snapshot();
    if (!fbSnapshot) {
        return false;
    }

    NetSnapshot snapshot;
    snapshot.sequence = fbSnapshot->sequence();

    if (const auto* actors = fbSnapshot->actors()) {
        if (actors->size() > kMaxActors) {
            return false;
        }
        snapshot.actors.reserve(actors->size());
        for (const mineworld::net::ActorState* actor : *actors) {
            if (!actor || !actor->name()) {
                return false;
            }
            NetActorState outActor;
            outActor.name = actor->name()->str();
            outActor.position = fromFbVec3(actor->position());
            outActor.velocity = fromFbVec3(actor->velocity());
            outActor.yaw = actor->yaw();
            outActor.pitch = actor->pitch();
            outActor.isPlayer = actor->is_player();
            outActor.playerMode = fromWirePlayerMode(actor->player_mode());
            outActor.lastInputSequence = actor->last_input_sequence();
            snapshot.actors.push_back(std::move(outActor));
        }
    }

    if (const auto* chunks = fbSnapshot->chunks()) {
        if (chunks->size() > kMaxChunks) {
            return false;
        }
        snapshot.chunks.reserve(chunks->size());
        for (const mineworld::net::ChunkState* chunk : *chunks) {
            if (!chunk) {
                return false;
            }
            NetChunkState outChunk;
            outChunk.chunkPos = fromFbIVec3(chunk->chunk_pos());
            outChunk.loaded = chunk->loaded();
            snapshot.chunks.push_back(outChunk);
        }
    }

    if (const auto* blocks = fbSnapshot->blocks()) {
        if (blocks->size() > kMaxBlocks) {
            return false;
        }
        snapshot.blocks.reserve(blocks->size());
        for (const mineworld::net::BlockState* block : *blocks) {
            if (!block) {
                return false;
            }
            NetBlockState outBlock;
            outBlock.worldPos = fromFbIVec3(block->world_pos());
            outBlock.data = BlockData{
                static_cast<BlockType>(block->type()),
                static_cast<BlockOrientation>(block->orientation()),
            };
            snapshot.blocks.push_back(outBlock);
        }
    }

    outSnapshot = std::move(snapshot);
    return true;
}
