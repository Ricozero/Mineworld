#include "chunk_data.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <utility>

namespace {

static_assert(std::endian::native == std::endian::little, "The index array is serialized as a raw little-endian uint32 blob");
static_assert(ChunkLayout::BLOCK_COUNT % 32 == 0, "Every supported index width must tile the index array into whole 32-bit words");

constexpr bool isValidIndexBits(uint8_t bits) {
    return bits == 1 || bits == 2 || bits == 4 || bits == 8;
}

// clang-format off
constexpr uint8_t indexShiftFor(uint8_t bits) {
    return bits == 1 ? 0 : bits == 2 ? 1 : bits == 4 ? 2 : 3;
}
// clang-format on

constexpr size_t wordCount(uint8_t bits) {
    return bits == 0 ? 0 : ChunkLayout::BLOCK_COUNT * bits / 32;
}

constexpr size_t paletteCapacity(uint8_t bits) {
    return size_t{1} << bits;
}

static_assert(static_cast<size_t>(BlockType::Count) * static_cast<size_t>(BlockOrientation::Count) <= paletteCapacity(ChunkData::MAX_INDEX_BITS));

constexpr uint8_t minIndexBits(size_t paletteSize) {
    if (paletteSize <= 2) return 1;
    if (paletteSize <= 4) return 2;
    if (paletteSize <= 16) return 4;
    return 8;
}

constexpr size_t kNoSlot = ~size_t{0};

void appendU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

uint16_t readU16(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset]) | static_cast<uint16_t>(bytes[offset + 1]) << 8);
}

}  // namespace

ChunkData::ChunkData(const ChunkData& other)
    : indexBits_(other.indexBits_),
      indexShift_(other.indexShift_),
      uniformBlock_(other.uniformBlock_),
      blockCount_(other.blockCount_),
      palette_(other.palette_) {
    if (other.data_) {
        const size_t words = wordCount(indexBits_);
        data_ = std::make_unique<uint32_t[]>(words);
        std::copy_n(other.data_.get(), words, data_.get());
    }
}

ChunkData& ChunkData::operator=(const ChunkData& other) {
    if (this != &other) {
        ChunkData copy(other);
        *this = std::move(copy);
    }
    return *this;
}

void ChunkData::fill(BlockData block) {
    uniformBlock_ = block;
    indexBits_ = 0;
    indexShift_ = 0;
    blockCount_ = block.type == BlockType::Air ? 0 : static_cast<uint16_t>(ChunkLayout::BLOCK_COUNT);
    palette_.clear();
    palette_.shrink_to_fit();
    data_.reset();
}

bool ChunkData::set(size_t index, BlockData block) {
    assert(index < ChunkLayout::BLOCK_COUNT);
    if (index >= ChunkLayout::BLOCK_COUNT || !isValidBlock(block)) {
        return false;
    }

    if (indexBits_ == 0) {
        if (block == uniformBlock_) {
            return false;
        }
        palette_.assign(1, PaletteEntry{uniformBlock_, static_cast<uint16_t>(ChunkLayout::BLOCK_COUNT)});
        rebuild(1, nullptr);
    }

    const uint16_t oldSlot = rawIndex(index);
    const BlockData oldBlock = palette_[oldSlot].block;
    if (oldBlock == block) {
        return false;
    }

    const uint16_t newSlot = findOrAddPaletteEntry(block);
    --palette_[oldSlot].count;
    ++palette_[newSlot].count;
    writeRawIndex(index, newSlot);

    if (oldBlock.type != BlockType::Air) {
        --blockCount_;
    }
    if (block.type != BlockType::Air) {
        ++blockCount_;
    }
    return true;
}

void ChunkData::optimize() {
    if (indexBits_ == 0) {
        return;
    }

    std::vector<uint16_t> counts(palette_.size(), 0);
    for (size_t index = 0; index < ChunkLayout::BLOCK_COUNT; ++index) {
        ++counts[rawIndex(index)];
    }

    std::vector<uint16_t> remap(palette_.size(), 0);
    std::vector<PaletteEntry> compact;
    compact.reserve(palette_.size());
    for (size_t slot = 0; slot < palette_.size(); ++slot) {
        if (counts[slot] == 0) {
            continue;
        }
        remap[slot] = static_cast<uint16_t>(compact.size());
        compact.push_back(PaletteEntry{palette_[slot].block, counts[slot]});
    }

    if (compact.size() == 1) {
        fill(compact[0].block);
        return;
    }

    blockCount_ = 0;
    for (const PaletteEntry& entry : compact) {
        if (entry.block.type != BlockType::Air) {
            blockCount_ += entry.count;
        }
    }

    const uint8_t bits = minIndexBits(compact.size());
    if (compact.size() != palette_.size() || bits != indexBits_) {
        rebuild(bits, remap.data());
    }
    palette_ = std::move(compact);
    palette_.shrink_to_fit();
}

void ChunkData::writeRawIndex(size_t index, uint16_t value) {
    const uint32_t offset = (static_cast<uint32_t>(index) & ((32u >> indexShift_) - 1u)) << indexShift_;
    const uint32_t mask = ((1u << indexBits_) - 1u) << offset;
    uint32_t& word = data_[index >> (5 - indexShift_)];
    word = (word & ~mask) | ((static_cast<uint32_t>(value) << offset) & mask);
}

void ChunkData::rebuild(uint8_t bits, const uint16_t* remap) {
    assert(isValidIndexBits(bits));
    const uint8_t newShift = indexShiftFor(bits);
    auto newData = std::make_unique<uint32_t[]>(wordCount(bits));

    if (indexBits_ != 0) {
        for (size_t index = 0; index < ChunkLayout::BLOCK_COUNT; ++index) {
            const uint16_t slot = rawIndex(index);
            const uint32_t value = remap != nullptr ? remap[slot] : slot;
            const uint32_t offset = (static_cast<uint32_t>(index) & ((32u >> newShift) - 1u)) << newShift;
            newData[index >> (5 - newShift)] |= value << offset;
        }
    }

    data_ = std::move(newData);
    indexBits_ = bits;
    indexShift_ = newShift;
}

uint16_t ChunkData::findOrAddPaletteEntry(BlockData block) {
    size_t freeSlot = kNoSlot;
    for (size_t slot = 0; slot < palette_.size(); ++slot) {
        if (palette_[slot].block == block) {
            return static_cast<uint16_t>(slot);
        }
        if (palette_[slot].count == 0 && freeSlot == kNoSlot) {
            freeSlot = slot;
        }
    }

    if (freeSlot != kNoSlot) {
        palette_[freeSlot].block = block;
        return static_cast<uint16_t>(freeSlot);
    }

    if (palette_.size() == paletteCapacity(indexBits_)) {
        if (indexBits_ >= MAX_INDEX_BITS) {
            assert(false && "palette overflow");
            return 0;
        }
        rebuild(static_cast<uint8_t>(indexBits_ * 2), nullptr);
    }

    palette_.push_back(PaletteEntry{block, 0});
    return static_cast<uint16_t>(palette_.size() - 1);
}

void ChunkData::serialize(std::vector<uint8_t>& out) const {
    out.clear();

    if (indexBits_ == 0) {
        out.reserve(SERIALIZED_HEADER_SIZE);
        out.push_back(static_cast<uint8_t>(Format::Uniform));
        appendU16(out, packBlock(uniformBlock_));
        return;
    }

    const size_t dataBytes = wordCount(indexBits_) * sizeof(uint32_t);
    out.reserve(SERIALIZED_HEADER_SIZE + palette_.size() * sizeof(uint16_t) + dataBytes);
    out.push_back(static_cast<uint8_t>(Format::Palette));
    out.push_back(indexBits_);
    out.push_back(static_cast<uint8_t>(palette_.size() - 1));
    for (const PaletteEntry& entry : palette_) {
        appendU16(out, packBlock(entry.block));
    }
    const auto* raw = reinterpret_cast<const uint8_t*>(data_.get());
    out.insert(out.end(), raw, raw + dataBytes);
}

bool ChunkData::deserialize(std::span<const uint8_t> bytes, ChunkData& out) {
    if (bytes.empty()) {
        return false;
    }

    if (bytes[0] == static_cast<uint8_t>(Format::Uniform)) {
        if (bytes.size() != SERIALIZED_HEADER_SIZE) {
            return false;
        }
        const BlockData block = unpackBlock(readU16(bytes, 1));
        if (!isValidBlock(block)) {
            return false;
        }
        out.fill(block);
        return true;
    }

    if (bytes[0] != static_cast<uint8_t>(Format::Palette) || bytes.size() < SERIALIZED_HEADER_SIZE) {
        return false;
    }

    const uint8_t bits = bytes[1];
    if (!isValidIndexBits(bits)) {
        return false;
    }
    const size_t paletteSize = static_cast<size_t>(bytes[2]) + 1;
    if (paletteSize > paletteCapacity(bits)) {
        return false;
    }

    const size_t paletteBytes = paletteSize * sizeof(uint16_t);
    const size_t dataBytes = wordCount(bits) * sizeof(uint32_t);
    if (bytes.size() != SERIALIZED_HEADER_SIZE + paletteBytes + dataBytes) {
        return false;
    }

    std::vector<PaletteEntry> palette(paletteSize);
    for (size_t slot = 0; slot < paletteSize; ++slot) {
        palette[slot].block = unpackBlock(readU16(bytes, SERIALIZED_HEADER_SIZE + slot * sizeof(uint16_t)));
        if (!isValidBlock(palette[slot].block)) {
            return false;
        }
    }

    ChunkData result;
    result.indexBits_ = bits;
    result.indexShift_ = indexShiftFor(bits);
    result.palette_ = std::move(palette);
    result.data_ = std::make_unique<uint32_t[]>(wordCount(bits));
    std::copy_n(bytes.data() + SERIALIZED_HEADER_SIZE + paletteBytes, dataBytes, reinterpret_cast<uint8_t*>(result.data_.get()));

    for (size_t index = 0; index < ChunkLayout::BLOCK_COUNT; ++index) {
        const uint16_t slot = result.rawIndex(index);
        if (slot >= paletteSize) {
            return false;
        }
        ++result.palette_[slot].count;
        if (result.palette_[slot].block.type != BlockType::Air) {
            ++result.blockCount_;
        }
    }

    out = std::move(result);
    return true;
}
