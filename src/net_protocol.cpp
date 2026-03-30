#include "net_protocol.h"

#include <cstring>

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/verifier.h>

#include "net_protocol_generated.h"

namespace {

constexpr size_t kMaxActors = 2048;
constexpr size_t kMaxChunks = 65536;
constexpr size_t kMaxBlocks = 65536;

flatbuffers::DetachedBuffer finish(flatbuffers::FlatBufferBuilder& builder,
                                   flatbuffers::Offset<mineworld::net::NetMessage> message) {
    // Generated helper includes the file identifier from the schema.
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

std::vector<uint8_t> serializeSnapshot(const NetSnapshot& snapshot) {
    flatbuffers::FlatBufferBuilder builder;

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
            &velocity));
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

    flatbuffers::DetachedBuffer buffer = finish(builder, msg);
    return std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size());
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
