#include "chunk.h"

#include <glm/gtx/string_cast.hpp>
#include <utility>

#include "log.h"

BlockData Chunk::getBlock(glm::ivec3 localPos) const {
    if (ChunkLayout::isValidLocalPosition(localPos)) {
        return data_.get(ChunkLayout::blockIndex(localPos));
    } else {
        return BlockData{};
    }
}

void Chunk::setBlock(glm::ivec3 localPos, BlockData blockData) {
    if (ChunkLayout::isValidLocalPosition(localPos)) {
        if (data_.set(ChunkLayout::blockIndex(localPos), blockData)) {
            ++revision_;
        }
    } else {
        logging::error("Attempted to set block at {}", glm::to_string(localToWorld(localPos)));
    }
}

void Chunk::clearBlock(glm::ivec3 localPos) {
    if (ChunkLayout::isValidLocalPosition(localPos)) {
        setBlock(localPos, BlockData{});
    } else {
        logging::error("Attempted to clear block at {}", glm::to_string(localToWorld(localPos)));
    }
}

bool Chunk::applyData(uint32_t revision, ChunkData&& data) {
    if (revision < revision_) {
        return false;
    }
    revision_ = revision;
    data_ = std::move(data);
    return true;
}
