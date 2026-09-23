#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <vector>

#include "chunk.h"
#include "chunk_test_support.h"

namespace {

using Clock = std::chrono::steady_clock;

using namespace test_support;

struct BlobInfo {
    uint8_t bits = 0;
    size_t paletteSize = 0;
};

BlobInfo inspect(const ChunkData& storage) {
    std::vector<uint8_t> blob;
    storage.serialize(blob);
    BlobInfo info;
    if (blob.size() >= 3 && blob[0] == static_cast<uint8_t>(ChunkData::Format::Palette)) {
        info.bits = blob[1];
        info.paletteSize = static_cast<size_t>(blob[2]) + 1;
    }
    return info;
}

constexpr size_t N = ChunkLayout::BLOCK_COUNT;

BlockData randomBlock(Rng& rng, uint32_t typeCount, uint32_t orientationCount) {
    return BlockData{static_cast<BlockType>(rng.below(typeCount)), static_cast<BlockOrientation>(rng.below(orientationCount))};
}

size_t countNonAir(const std::vector<uint16_t>& ref) {
    size_t count = 0;
    for (uint16_t packed : ref) {
        if (unpackBlock(packed).type != BlockType::Air) {
            ++count;
        }
    }
    return count;
}

testing::AssertionResult sameContents(const ChunkData& storage, const std::vector<uint16_t>& ref) {
    for (size_t i = 0; i < N; ++i) {
        const auto actual = packBlock(storage.get(i));
        if (actual != ref[i]) {
            return testing::AssertionFailure() << "Block index: " << i << ", actual: " << actual << ", expected: " << ref[i];
        }
    }
    return testing::AssertionSuccess();
}

void testStorage(uint32_t typeCount, uint32_t orientationCount, uint64_t seed, const char* label) {
    SCOPED_TRACE(testing::Message() << label << ", seed: " << seed);
    Rng rng{seed};
    ChunkData storage;
    std::vector<uint16_t> ref(N, packBlock(BlockData{}));

    ASSERT_TRUE(storage.isUniform()) << "fresh storage is uniform";
    ASSERT_EQ(storage.blockCount(), 0) << "fresh storage has no solid blocks";
    ASSERT_TRUE(sameContents(storage, ref)) << "fresh storage reads as air";

    for (int iteration = 0; iteration < 400000; ++iteration) {
        const size_t index = rng.below(static_cast<uint32_t>(N));
        const BlockData block = randomBlock(rng, typeCount, orientationCount);
        const uint16_t packed = packBlock(block);
        const bool changed = storage.set(index, block);
        ASSERT_EQ(changed, (ref[index] != packed)) << "Iteration: " << iteration << ", block index: " << index;
        ref[index] = packed;

        if (iteration % 20000 == 0) {
            SCOPED_TRACE(testing::Message() << "Iteration: " << iteration);
            ASSERT_TRUE(sameContents(storage, ref)) << "contents match the reference";
            ASSERT_EQ(storage.blockCount(), countNonAir(ref)) << "nonAirCount matches the reference";

            std::vector<uint8_t> blob;
            storage.serialize(blob);
            ChunkData restored;
            ASSERT_TRUE(ChunkData::deserialize(blob, restored)) << "serialize output round-trips";
            ASSERT_TRUE(sameContents(restored, ref)) << "round-tripped contents match";
            ASSERT_EQ(restored.blockCount(), countNonAir(ref)) << "round-tripped nonAirCount matches";
            ASSERT_EQ(restored.isUniform(), storage.isUniform()) << "round-tripped uniform flag matches";

            storage.optimize();
            ASSERT_TRUE(sameContents(storage, ref)) << "optimize() preserves contents";
            ASSERT_EQ(storage.blockCount(), countNonAir(ref)) << "optimize() preserves nonAirCount";
            const size_t distinct = [&] {
                std::array<bool, 256> seen{};
                size_t total = 0;
                for (uint16_t packedRef : ref) {
                    const size_t key = (static_cast<size_t>(packedRef >> 8) * 6) + (packedRef & 0xFF);
                    if (!seen[key]) {
                        seen[key] = true;
                        ++total;
                    }
                }
                return total;
            }();
            if (distinct == 1) {
                ASSERT_TRUE(storage.isUniform()) << "optimize() collapses a single-block chunk";
            } else {
                const BlobInfo info = inspect(storage);
                ASSERT_EQ(info.paletteSize, distinct) << "optimize() keeps exactly the used palette entries";
                ASSERT_GE(static_cast<size_t>(1) << info.bits, distinct) << "index width addresses the palette";
                ASSERT_TRUE(info.bits / 2 == 0 || (static_cast<size_t>(1) << (info.bits / 2)) < distinct) << "index width is the smallest that fits";
            }
        }
    }
}

TEST(ChunkDataTest, RandomizedStorageMatchesReference) {
    ASSERT_NO_FATAL_FAILURE(testStorage(2, 1, 1, "2 blocks"));
    ASSERT_NO_FATAL_FAILURE(testStorage(3, 1, 2, "3 blocks"));
    ASSERT_NO_FATAL_FAILURE(testStorage(5, 2, 3, "10 blocks"));
    ASSERT_NO_FATAL_FAILURE(testStorage(static_cast<uint32_t>(BlockType::Count), static_cast<uint32_t>(BlockOrientation::Count), 4, "48 blocks"));
}

TEST(ChunkDataTest, UniformAndMalformedBlobs) {
    ChunkData storage;
    storage.fill(BlockData{BlockType::Stone, BlockOrientation::North});
    EXPECT_TRUE(storage.isUniform()) << "fill() produces a uniform chunk";
    EXPECT_EQ(storage.blockCount(), N) << "a uniform solid chunk counts every block";
    EXPECT_EQ(inspect(storage).bits, 0) << "a uniform chunk keeps the uniform form";

    std::vector<uint8_t> blob;
    storage.serialize(blob);
    EXPECT_EQ(blob.size(), 3) << "a uniform chunk serializes to 3 bytes";
    ChunkData restored;
    ASSERT_TRUE(ChunkData::deserialize(blob, restored)) << "uniform blob round-trips";
    ASSERT_TRUE(restored.isUniform()) << "uniform blob keeps its block";
    ASSERT_EQ(packBlock(restored.uniformBlock()), packBlock(storage.uniformBlock())) << "uniform blob keeps its block";

    storage.set(100, BlockData{BlockType::Dirt, BlockOrientation::North});
    EXPECT_FALSE(storage.isUniform()) << "editing a uniform chunk leaves the uniform form";
    storage.set(100, BlockData{BlockType::Stone, BlockOrientation::North});
    storage.optimize();
    EXPECT_TRUE(storage.isUniform()) << "optimize() returns to the uniform form";

    ChunkData sink;
    EXPECT_FALSE((ChunkData::deserialize({}, sink))) << "an empty blob is rejected";
    EXPECT_FALSE((ChunkData::deserialize(std::vector<uint8_t>{9, 0, 0}, sink))) << "an unknown format byte is rejected";
    EXPECT_FALSE((ChunkData::deserialize(std::vector<uint8_t>{0, 0}, sink))) << "a truncated uniform blob is rejected";
    EXPECT_FALSE((ChunkData::deserialize(std::vector<uint8_t>{0, 0, static_cast<uint8_t>(BlockType::Count)}, sink))) << "a uniform blob with an invalid block is rejected";

    std::vector<uint8_t> badBits{1, 3, 0, 0, 0};
    badBits.resize(3 + 2 + N * 3 / 8, 0);
    EXPECT_FALSE(ChunkData::deserialize(badBits, sink)) << "an unsupported index width is rejected";

    std::vector<uint8_t> tooBig{1, 1, 2};
    tooBig.resize(3 + 3 * 2 + N / 8, 0);
    EXPECT_FALSE(ChunkData::deserialize(tooBig, sink)) << "a palette larger than the index width allows is rejected";

    std::vector<uint8_t> shortBlob{1, 1, 1, 0, 0, 0, 0};
    EXPECT_FALSE(ChunkData::deserialize(shortBlob, sink)) << "a truncated palette blob is rejected";

    std::vector<uint8_t> outOfRange{1, 1, 0, 0, 0};
    outOfRange.resize(3 + 2 + N / 8, 0);
    outOfRange.back() = 0x80;
    EXPECT_FALSE(ChunkData::deserialize(outOfRange, sink)) << "an out-of-range index is rejected";

    std::vector<uint8_t> badEntry{1, 1, 1, 0, 0, static_cast<uint8_t>(BlockType::Count), 0};
    badEntry.resize(3 + 4 + N / 8, 0);
    EXPECT_FALSE(ChunkData::deserialize(badEntry, sink)) << "an invalid palette entry is rejected";
}

TEST(ChunkCodecTest, RoundTripCorruptionAndCacheInvalidation) {
    for (const auto& data : {ChunkData{}, ChunkData{BlockType::Stone}, patternedData(false), patternedData(true)}) {
        const auto encoded = ChunkCodec::encode(data);
        ChunkData decoded;
        ASSERT_TRUE(ChunkCodec::decode(encoded.compression, encoded.uncompressedSize, encoded.bytes, decoded)) << "codec round trip failed";
        ASSERT_NO_FATAL_FAILURE(sameBlocks(data, decoded));
        if (data.isUniform()) {
            ASSERT_EQ(encoded.compression, ChunkCompression::None) << "uniform data should remain raw";
            ASSERT_EQ(encoded.bytes.size(), 3) << "uniform data should remain raw";
        }
        ASSERT_FALSE(ChunkCodec::decode(encoded.compression, encoded.uncompressedSize + 1, encoded.bytes, decoded)) << "accepted wrong decoded length";
        ASSERT_FALSE(ChunkCodec::decode(static_cast<ChunkCompression>(255), encoded.uncompressedSize, encoded.bytes, decoded)) << "accepted unknown compression";
        ASSERT_FALSE(ChunkCodec::decode(encoded.compression, ChunkData::MAX_SERIALIZED_SIZE + 1, encoded.bytes, decoded)) << "accepted oversized decoded length";
        ASSERT_NO_FATAL_FAILURE(sameBlocks(data, decoded));
    }
    ASSERT_EQ(ChunkCodec::encode(patternedData(false)).compression, ChunkCompression::Lz4) << "pattern did not compress";
    ASSERT_EQ(ChunkCodec::encode(patternedData(true)).compression, ChunkCompression::None) << "incompressible data did not stay raw";
    ChunkData unchanged{BlockType::Stone};
    const std::vector<uint8_t> invalid{0xff};
    ASSERT_FALSE(ChunkCodec::decode(ChunkCompression::Lz4, 100, invalid, unchanged)) << "accepted corrupt LZ4";
    ASSERT_TRUE(unchanged.isUniform()) << "failed decode modified output";
    ASSERT_EQ(unchanged.uniformBlock().type, BlockType::Stone) << "failed decode modified output";
    const std::vector<uint8_t> invalidBlock{0, 255, 255};
    ASSERT_FALSE(ChunkCodec::decode(ChunkCompression::None, 3, invalidBlock, unchanged)) << "accepted invalid block";

    Chunk chunk({0, 0, 0});
    ASSERT_TRUE(chunk.applyData(1, patternedData(false))) << "initial apply failed";
    const auto first = chunk.getEncodedSnapshot();
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(first, chunk.getEncodedSnapshot()) << "cache not shared";
    chunk.setBlock({0, 0, 0}, chunk.getBlock({0, 0, 0}));
    ASSERT_EQ(first, chunk.getEncodedSnapshot()) << "no-op invalidated cache";
    chunk.setBlock({0, 0, 0}, BlockData{BlockType::Water});
    const auto changed = chunk.getEncodedSnapshot();
    ASSERT_NE(changed, nullptr);
    ASSERT_NE(changed, first) << "mutation cache mismatch";
    ASSERT_EQ(changed->revision, 2) << "mutation cache mismatch";
    ASSERT_EQ(first->revision, 1) << "mutation cache mismatch";
    ASSERT_FALSE((chunk.applyData(1, ChunkData{}))) << "accepted stale revision";
    ASSERT_EQ(changed, chunk.getEncodedSnapshot()) << "rejected apply invalidated cache";
    ASSERT_TRUE((chunk.applyData(2, ChunkData{BlockType::Sand}))) << "equal revision apply failed";
    ASSERT_NE(changed, chunk.getEncodedSnapshot()) << "equal revision did not invalidate cache";
    chunk.clearBlock({0, 0, 0});
    ASSERT_EQ(chunk.getEncodedSnapshot()->revision, 3) << "clearBlock did not invalidate cache";
    Chunk reloaded({0, 0, 0});
    reloaded.applyData(1, ChunkData{BlockType::Water});
    ASSERT_NE(first, reloaded.getEncodedSnapshot()) << "reloaded chunk reused old lifetime snapshot";
    ChunkData oldDecoded;
    ASSERT_TRUE(ChunkCodec::decode(first->data.compression, first->data.uncompressedSize, first->data.bytes, oldDecoded)) << "retained snapshot corrupted";
    ASSERT_NO_FATAL_FAILURE(sameBlocks(oldDecoded, patternedData(false)));
}

TEST(ChunkCodecBenchmark, ConcentratedEncodeDecodeBurst) {
    const auto data = patternedData(false);
    double encodeTotal = 0.0, encodePeak = 0.0, decodeTotal = 0.0, decodePeak = 0.0;
    size_t rawBytes = 0, encodedBytes = 0;
    constexpr int count = 2048;
    for (int i = 0; i < count; ++i) {
        const auto start = Clock::now();
        const auto encoded = ChunkCodec::encode(data);
        const auto encodedAt = Clock::now();
        ChunkData decoded;
        ASSERT_TRUE(ChunkCodec::decode(encoded.compression, encoded.uncompressedSize, encoded.bytes, decoded)) << "benchmark decode failed";
        const auto done = Clock::now();
        const double encodeMs = std::chrono::duration<double, std::milli>(encodedAt - start).count();
        const double decodeMs = std::chrono::duration<double, std::milli>(done - encodedAt).count();
        encodeTotal += encodeMs;
        encodePeak = std::max(encodePeak, encodeMs);
        decodeTotal += decodeMs;
        decodePeak = std::max(decodePeak, decodeMs);
        rawBytes += encoded.uncompressedSize;
        encodedBytes += encoded.bytes.size();
    }
    std::cout << "Codec burst (" << count << " patterned chunks): raw=" << rawBytes << " encoded=" << encodedBytes << " encode total/peak ms=" << encodeTotal << '/' << encodePeak << " decode total/peak ms=" << decodeTotal << '/' << decodePeak << '\n';
}

}  // namespace
