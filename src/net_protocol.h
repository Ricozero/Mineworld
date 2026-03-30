#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <string>
#include <vector>

#include "block.h"

struct NetActorState {
    std::string name;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
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

std::vector<uint8_t> serializeClientHello();
bool deserializeClientHello(std::span<const uint8_t> bytes);

std::vector<uint8_t> serializeSnapshot(const NetSnapshot& snapshot);
bool deserializeSnapshot(std::span<const uint8_t> bytes, NetSnapshot& outSnapshot);
