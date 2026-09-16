#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chunk_data.h"

enum class ChunkCompression : uint8_t {
    None,
    Lz4,
    Count,
};

struct EncodedChunkData {
    ChunkCompression compression = ChunkCompression::None;
    uint32_t uncompressedSize = 0;
    std::vector<uint8_t> bytes;
};

class ChunkCodec {
public:
    static EncodedChunkData encode(const ChunkData& data);
    static bool decode(ChunkCompression compression, uint32_t uncompressedSize, std::span<const uint8_t> bytes, ChunkData& out);
};
