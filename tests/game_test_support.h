#pragma once

#include <gtest/gtest.h>

#include <deque>
#include <unordered_set>
#include <utility>

#include "actor_world.h"
#include "config.h"
#include "net_interface.h"
#include "net_protocol.h"
#include "system.h"
#include "test_support.h"
#include "voxel_world.h"

namespace test_support {

using Payload = mineworld::net::NetMessagePayload;

class ScopedAppConfig {
public:
    ScopedAppConfig() = default;
    ~ScopedAppConfig() { AppConfig::instance() = std::move(saved_); }
    ScopedAppConfig(const ScopedAppConfig&) = delete;
    ScopedAppConfig& operator=(const ScopedAppConfig&) = delete;

private:
    AppConfig saved_ = AppConfig::instance();
};

class GameTest : public testing::Test {
protected:
    void SetUp() override {
        ensureLogger();
        auto& config = AppConfig::instance();
        config.entityViewRadius = 64.0f;
        config.chunkViewRadiusHorizontal = 1;
        config.chunkViewRadiusVertical = 1;
        config.spawnPosition = {0.0f, 128.0f, 0.0f};
    }

private:
    ScopedAppConfig config_;
};

class FakeServer final : public INetServer {
public:
    void connect(uint32_t id) {
        sessions.insert(id);
        events.push_back(NetEvent{NetEventType::Connected, id, {}});
        receive(id, serializeClientHello());
    }
    void receive(uint32_t id, std::vector<uint8_t> bytes) {
        events.push_back(NetEvent{NetEventType::Packet, id, std::move(bytes)});
    }
    bool send(uint32_t id, std::span<const uint8_t> bytes) override {
        if (!sessions.contains(id) || getPacketType(bytes) == rejectedType) {
            lastRejected.assign(bytes.begin(), bytes.end());
            return false;
        }
        sent.push_back(NetEvent{NetEventType::Packet, id, {bytes.begin(), bytes.end()}});
        return true;
    }
    void flush() override {}
    void pump() override {}
    bool popEvent(NetEvent& event) override {
        if (events.empty()) return false;
        event = std::move(events.front());
        events.pop_front();
        return true;
    }
    void close(uint32_t id) override {
        if (!sessions.erase(id)) return;
        std::erase_if(events, [id](const NetEvent& event) { return event.sessionId == id; });
        events.push_back(NetEvent{NetEventType::Disconnected, id, {}});
    }
    bool hasSession(uint32_t id) const override { return sessions.contains(id); }

    Payload rejectedType = Payload::NONE;
    std::vector<uint8_t> lastRejected;
    std::vector<NetEvent> sent;
    std::deque<NetEvent> events;
    std::unordered_set<uint32_t> sessions;
};

class ActorWorldProbe final : public System {
public:
    void update(VoxelWorld& voxels, ActorWorld& actors, float) override {
        world = &actors;
        terrain = &voxels;
    }
    ActorWorld* world = nullptr;
    VoxelWorld* terrain = nullptr;
};

inline testing::AssertionResult latestActorSnapshot(const FakeServer& wire, uint32_t sessionId, NetEntitySnapshot& snapshot) {
    for (auto it = wire.sent.rbegin(); it != wire.sent.rend(); ++it) {
        if (it->sessionId == sessionId && deserializeEntitySnapshot(it->payload, snapshot)) {
            return testing::AssertionSuccess();
        }
    }
    return testing::AssertionFailure() << "No entity snapshot received for session " << sessionId;
}

}  // namespace test_support
