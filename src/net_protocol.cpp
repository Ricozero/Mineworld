#include "net_protocol.h"

#include <cstddef>
#include <cstring>
#include <limits>

namespace {

class ByteWriter {
public:
    void writeU8(uint8_t value) {
        data_.push_back(value);
    }

    void writeU32(uint32_t value) {
        data_.push_back(static_cast<uint8_t>(value & 0xFFu));
        data_.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
        data_.push_back(static_cast<uint8_t>((value >> 16u) & 0xFFu));
        data_.push_back(static_cast<uint8_t>((value >> 24u) & 0xFFu));
    }

    void writeI32(int32_t value) {
        writeU32(static_cast<uint32_t>(value));
    }

    void writeF32(float value) {
        static_assert(sizeof(float) == sizeof(uint32_t));
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(float));
        writeU32(bits);
    }

    void writeString(const std::string& value) {
        writeU32(static_cast<uint32_t>(value.size()));
        data_.insert(data_.end(), value.begin(), value.end());
    }

    std::vector<uint8_t> take() {
        return std::move(data_);
    }

private:
    std::vector<uint8_t> data_;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const uint8_t> data) : data_(data) {}

    bool readU8(uint8_t& out) {
        if (!ensure(1)) return false;
        out = data_[offset_++];
        return true;
    }

    bool readU32(uint32_t& out) {
        if (!ensure(4)) return false;
        out = static_cast<uint32_t>(data_[offset_]) |
              (static_cast<uint32_t>(data_[offset_ + 1]) << 8u) |
              (static_cast<uint32_t>(data_[offset_ + 2]) << 16u) |
              (static_cast<uint32_t>(data_[offset_ + 3]) << 24u);
        offset_ += 4;
        return true;
    }

    bool readI32(int32_t& out) {
        uint32_t value = 0;
        if (!readU32(value)) return false;
        std::memcpy(&out, &value, sizeof(int32_t));
        return true;
    }

    bool readF32(float& out) {
        uint32_t bits = 0;
        if (!readU32(bits)) return false;
        std::memcpy(&out, &bits, sizeof(float));
        return true;
    }

    bool readString(std::string& out) {
        uint32_t size = 0;
        if (!readU32(size)) return false;
        if (!ensure(size)) return false;
        out.assign(reinterpret_cast<const char*>(data_.data() + offset_), size);
        offset_ += size;
        return true;
    }

    bool isEnd() const {
        return offset_ == data_.size();
    }

private:
    bool ensure(size_t count) const {
        if (count > std::numeric_limits<size_t>::max() - offset_) {
            return false;
        }
        return offset_ + count <= data_.size();
    }

    std::span<const uint8_t> data_;
    size_t offset_ = 0;
};

void writeVec3(ByteWriter& writer, const glm::vec3& value) {
    writer.writeF32(value.x);
    writer.writeF32(value.y);
    writer.writeF32(value.z);
}

bool readVec3(ByteReader& reader, glm::vec3& out) {
    return reader.readF32(out.x) && reader.readF32(out.y) && reader.readF32(out.z);
}

void writeIVec3(ByteWriter& writer, const glm::ivec3& value) {
    writer.writeI32(value.x);
    writer.writeI32(value.y);
    writer.writeI32(value.z);
}

bool readIVec3(ByteReader& reader, glm::ivec3& out) {
    return reader.readI32(out.x) && reader.readI32(out.y) && reader.readI32(out.z);
}

}  // namespace

std::vector<uint8_t> serializeClientHello() {
    ByteWriter writer;
    writer.writeU8(static_cast<uint8_t>(NetMessageType::ClientHello));
    return writer.take();
}

bool deserializeClientHello(std::span<const uint8_t> bytes) {
    ByteReader reader(bytes);
    uint8_t messageType = 0;
    return reader.readU8(messageType) &&
           messageType == static_cast<uint8_t>(NetMessageType::ClientHello) &&
           reader.isEnd();
}

std::vector<uint8_t> serializeSnapshot(const NetSnapshot& snapshot) {
    ByteWriter writer;
    writer.writeU8(static_cast<uint8_t>(NetMessageType::Snapshot));
    writer.writeU32(snapshot.sequence);

    writer.writeU32(static_cast<uint32_t>(snapshot.actors.size()));
    for (const auto& actor : snapshot.actors) {
        writer.writeString(actor.name);
        writeVec3(writer, actor.position);
        writeVec3(writer, actor.velocity);
    }

    writer.writeU32(static_cast<uint32_t>(snapshot.chunks.size()));
    for (const auto& chunk : snapshot.chunks) {
        writeIVec3(writer, chunk.chunkPos);
        writer.writeU8(chunk.loaded ? 1 : 0);
    }

    writer.writeU32(static_cast<uint32_t>(snapshot.blocks.size()));
    for (const auto& block : snapshot.blocks) {
        writeIVec3(writer, block.worldPos);
        writer.writeU8(static_cast<uint8_t>(block.data.type));
        writer.writeU8(static_cast<uint8_t>(block.data.orientation));
    }

    return writer.take();
}

bool deserializeSnapshot(std::span<const uint8_t> bytes, NetSnapshot& outSnapshot) {
    ByteReader reader(bytes);

    uint8_t messageType = 0;
    if (!reader.readU8(messageType) ||
        messageType != static_cast<uint8_t>(NetMessageType::Snapshot)) {
        return false;
    }

    NetSnapshot snapshot;
    if (!reader.readU32(snapshot.sequence)) {
        return false;
    }

    uint32_t actorCount = 0;
    if (!reader.readU32(actorCount)) {
        return false;
    }
    snapshot.actors.reserve(actorCount);
    for (uint32_t i = 0; i < actorCount; ++i) {
        NetActorState actor;
        if (!reader.readString(actor.name) ||
            !readVec3(reader, actor.position) ||
            !readVec3(reader, actor.velocity)) {
            return false;
        }
        snapshot.actors.push_back(std::move(actor));
    }

    uint32_t chunkCount = 0;
    if (!reader.readU32(chunkCount)) {
        return false;
    }
    snapshot.chunks.reserve(chunkCount);
    for (uint32_t i = 0; i < chunkCount; ++i) {
        NetChunkState chunk;
        uint8_t loaded = 0;
        if (!readIVec3(reader, chunk.chunkPos) || !reader.readU8(loaded)) {
            return false;
        }
        chunk.loaded = loaded != 0;
        snapshot.chunks.push_back(chunk);
    }

    uint32_t blockCount = 0;
    if (!reader.readU32(blockCount)) {
        return false;
    }
    snapshot.blocks.reserve(blockCount);
    for (uint32_t i = 0; i < blockCount; ++i) {
        NetBlockState block;
        uint8_t type = 0;
        uint8_t orientation = 0;
        if (!readIVec3(reader, block.worldPos) ||
            !reader.readU8(type) ||
            !reader.readU8(orientation)) {
            return false;
        }
        block.data = BlockData{
            static_cast<BlockType>(type),
            static_cast<BlockOrientation>(orientation),
        };
        snapshot.blocks.push_back(block);
    }

    if (!reader.isEnd()) {
        return false;
    }

    outSnapshot = std::move(snapshot);
    return true;
}
