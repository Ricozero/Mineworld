#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "block.h"
#include "chunk_layout.h"

class ChunkData {
public:
    static constexpr uint8_t MAX_INDEX_BITS = 8;

    enum class Format : uint8_t {
        Uniform = 0,
        Palette = 1,
    };

    ChunkData() = default;
    explicit ChunkData(BlockData block) { fill(block); }
    ChunkData(const ChunkData& other);
    ChunkData& operator=(const ChunkData& other);
    ChunkData(ChunkData&&) noexcept = default;
    ChunkData& operator=(ChunkData&&) noexcept = default;
    ~ChunkData() = default;

    BlockData get(size_t index) const {
        assert(index < ChunkLayout::BLOCK_COUNT);
        return indexBits_ == 0 ? uniformBlock_ : palette_[rawIndex(index)].block;
    }
    bool set(size_t index, BlockData block);
    void fill(BlockData block);
    void optimize();

    bool isUniform() const { return indexBits_ == 0; }
    BlockData uniformBlock() const { return uniformBlock_; }
    size_t blockCount() const { return blockCount_; }

    static constexpr size_t SERIALIZED_HEADER_SIZE = 3;
    static constexpr size_t MAX_SERIALIZED_SIZE = SERIALIZED_HEADER_SIZE + (size_t{1} << MAX_INDEX_BITS) * sizeof(uint16_t) + ChunkLayout::BLOCK_COUNT;

    void serialize(std::vector<uint8_t>& out) const;
    static bool deserialize(std::span<const uint8_t> bytes, ChunkData& out);

private:
    struct PaletteEntry {
        BlockData block;
        uint16_t count = 0;
    };

    uint16_t rawIndex(size_t index) const {
        const uint32_t shift = indexShift_;
        const uint32_t word = data_[index >> (5 - shift)];
        const uint32_t offset = (static_cast<uint32_t>(index) & ((32u >> shift) - 1u)) << shift;
        return static_cast<uint16_t>((word >> offset) & ((1u << indexBits_) - 1u));
    }
    void writeRawIndex(size_t index, uint16_t value);
    void rebuild(uint8_t bits, const uint16_t* remap);
    uint16_t findOrAddPaletteEntry(BlockData block);

    uint8_t indexBits_ = 0;
    uint8_t indexShift_ = 0;
    BlockData uniformBlock_;
    uint16_t blockCount_ = 0;
    std::vector<PaletteEntry> palette_;
    std::unique_ptr<uint32_t[]> data_;
};
