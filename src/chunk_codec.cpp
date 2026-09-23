#include "chunk_codec.h"

#include <lz4.h>

#include <array>

#include "profiler.h"

namespace {

constexpr size_t kCompressionThreshold = 256;
constexpr size_t kMinimumSavings = 32;
constexpr ChunkCompression kCompressionMode = ChunkCompression::Lz4;

}  // namespace

EncodedChunkData ChunkCodec::encode(const ChunkData& data) {
    EncodedChunkData result;
    data.serialize(result.bytes);
    result.uncompressedSize = static_cast<uint32_t>(result.bytes.size());
    if (kCompressionMode == ChunkCompression::Lz4 && result.bytes.size() >= kCompressionThreshold) {
        std::array<uint8_t, LZ4_COMPRESSBOUND(ChunkData::kMaxSerializedSize)> compressed;
        const int size = LZ4_compress_default(
            reinterpret_cast<const char*>(result.bytes.data()), reinterpret_cast<char*>(compressed.data()),
            static_cast<int>(result.bytes.size()), LZ4_compressBound(static_cast<int>(result.bytes.size())));
        if (size > 0 && static_cast<size_t>(size) + kMinimumSavings <= result.bytes.size()) {
            result.bytes = std::vector<uint8_t>(compressed.begin(), compressed.begin() + size);
            result.compression = ChunkCompression::Lz4;
        }
    }
    MW_PROFILE_COUNTER("ChunkCodec.RawBytes", result.uncompressedSize);
    MW_PROFILE_COUNTER("ChunkCodec.EncodedBytes", static_cast<int64_t>(result.bytes.size()));
    return result;
}

bool ChunkCodec::decode(ChunkCompression compression, uint32_t uncompressedSize, std::span<const uint8_t> bytes, ChunkData& out) {
    if (uncompressedSize < ChunkData::kSerializedHeaderSize || uncompressedSize > ChunkData::kMaxSerializedSize ||
        bytes.empty() || bytes.size() > ChunkData::kMaxSerializedSize) {
        return false;
    }
    switch (compression) {
        case ChunkCompression::None:
            return bytes.size() == uncompressedSize && ChunkData::deserialize(bytes, out);
        case ChunkCompression::Lz4: {
            std::array<uint8_t, ChunkData::kMaxSerializedSize> raw;
            const int size = LZ4_decompress_safe(
                reinterpret_cast<const char*>(bytes.data()), reinterpret_cast<char*>(raw.data()),
                static_cast<int>(bytes.size()), static_cast<int>(uncompressedSize));
            return size == static_cast<int>(uncompressedSize) && ChunkData::deserialize(std::span(raw.data(), uncompressedSize), out);
        }
        default:
            return false;
    }
}
