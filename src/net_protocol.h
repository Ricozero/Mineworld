#pragma once

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <string>
#include <vector>

#include "chunk.h"
#include "command.h"
#include "entity.h"
#include "net_protocol_generated.h"

struct NetActorState {
    std::string name;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    EntityType entityType = EntityType::Player;
    PlayerMode playerMode = PlayerMode::Survival;
};

struct NetEntitySnapshot {
    uint32_t sequence = 0;
    std::vector<NetActorState> actors;
};

inline constexpr size_t MAX_CHUNK_BATCH_BYTES = 32 * 1024;
inline constexpr size_t MAX_CHUNK_UPSERTS_PER_BATCH = 64;
inline constexpr size_t MAX_CHUNK_UNLOADS_PER_BATCH = 256;

struct NetChunkUpsert {
    glm::ivec3 chunkPos{0};
    std::shared_ptr<const EncodedChunkSnapshot> snapshot;
};

struct NetDecodedChunkUpsert {
    glm::ivec3 chunkPos{0};
    uint32_t revision = 0;
    ChunkData blocks;
};

struct NetChunkUnload {
    glm::ivec3 chunkPos{0};
    uint32_t revision = 0;
};

struct NetClientInput {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    PlayerMode playerMode = PlayerMode::Survival;
    uint32_t sequence = 0;
};

struct NetServerHello {
    uint32_t sessionId = 0;
    std::string actorName;
    glm::vec3 position{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    PlayerMode playerMode = PlayerMode::Survival;
    std::vector<glm::ivec3> coreChunks;
};

mineworld::net::NetMessagePayload getPacketType(std::span<const uint8_t> bytes);

std::vector<uint8_t> serializeClientHello();
std::vector<uint8_t> serializeClientDisconnect();
std::vector<uint8_t> serializeClientReady();

std::vector<uint8_t> serializeServerHello(const NetServerHello& hello);
bool deserializeServerHello(std::span<const uint8_t> bytes, NetServerHello& outHello);

std::vector<uint8_t> serializeClientInput(const NetClientInput& input);
bool deserializeClientInput(std::span<const uint8_t> bytes, NetClientInput& outInput);

std::vector<uint8_t> serializeEntitySnapshot(const NetEntitySnapshot& snapshot, flatbuffers::FlatBufferBuilder& builder);
bool deserializeEntitySnapshot(std::span<const uint8_t> bytes, NetEntitySnapshot& outSnapshot);

std::vector<uint8_t> serializeChunkUpsertBatch(std::span<const NetChunkUpsert> chunks, flatbuffers::FlatBufferBuilder& builder);
bool deserializeChunkUpsertBatch(std::span<const uint8_t> bytes, std::vector<NetDecodedChunkUpsert>& outChunks);

std::vector<uint8_t> serializeChunkUnloadBatch(std::span<const NetChunkUnload> chunks, flatbuffers::FlatBufferBuilder& builder);
bool deserializeChunkUnloadBatch(std::span<const uint8_t> bytes, std::vector<NetChunkUnload>& outChunks);

std::vector<uint8_t> serializeCommandRequest(const CommandRequest& command);
bool deserializeCommandRequest(std::span<const uint8_t> bytes, CommandRequest& outCommand);

std::vector<uint8_t> serializeCommandResponse(const CommandResponse& response);
bool deserializeCommandResponse(std::span<const uint8_t> bytes, CommandResponse& outResponse);
