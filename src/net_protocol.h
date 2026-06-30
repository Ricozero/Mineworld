#pragma once

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <string>
#include <vector>

#include "block.h"
#include "entity.h"

struct NetActorState {
    std::string name;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool isPlayer = false;
    PlayerMode playerMode = PlayerMode::Survival;
    uint32_t lastInputSequence = 0;
    bool isGrounded = false;
};

struct NetChunkState {
    glm::ivec3 chunkPos{0};
    bool loaded = false;
};

struct NetBlockState {
    glm::ivec3 worldPos{0};
    BlockData data{};
};

struct NetSnapshot {
    uint32_t sequence = 0;
    std::vector<NetActorState> actors;
    std::vector<NetChunkState> chunks;
    std::vector<NetBlockState> blocks;
};

struct NetClientInput {
    glm::vec3 move{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    PlayerMode playerMode = PlayerMode::Survival;
    bool jump = false;
    bool sprint = false;
    uint32_t sequence = 0;
    float deltaTime = 0.0f;
};

struct NetServerHello {
    uint32_t sessionId = 0;
    std::string actorName;
    glm::vec3 position{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    PlayerMode playerMode = PlayerMode::Survival;
};

std::vector<uint8_t> serializeClientHello();
bool deserializeClientHello(std::span<const uint8_t> bytes);

std::vector<uint8_t> serializeClientDisconnect();
bool deserializeClientDisconnect(std::span<const uint8_t> bytes);

std::vector<uint8_t> serializeServerHello(const NetServerHello& hello);
bool deserializeServerHello(std::span<const uint8_t> bytes, NetServerHello& outHello);

std::vector<uint8_t> serializeClientInput(const NetClientInput& input);
bool deserializeClientInput(std::span<const uint8_t> bytes, NetClientInput& outInput);

std::vector<uint8_t> serializeSnapshot(const NetSnapshot& snapshot, flatbuffers::FlatBufferBuilder& builder);
bool deserializeSnapshot(std::span<const uint8_t> bytes, NetSnapshot& outSnapshot);
