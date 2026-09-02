#pragma once

#include <flatbuffers/flatbuffers.h>

#include <asio.hpp>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "actor_world.h"
#include "net_interface.h"
#include "net_protocol.h"
#include "server_chunk_manager.h"
#include "system.h"
#include "voxel_world.h"

class GameServer {
public:
    GameServer();
    ~GameServer();

    void registerSystem(std::unique_ptr<System> system);
    void update(float deltaTime);

    entt::entity createLocalPlayer(const std::string& name, uint32_t sessionId, glm::vec3 position, PlayerMode mode);
    entt::entity createRobot(const std::string& name, glm::vec3 position);

private:
    struct Session {
        uint32_t sessionId = 0;
        uint32_t entitySnapshotSequence = 0;
        float entitySnapshotTimer = 0.0f;
        bool helloReceived = false;
        bool ready = false;
        uint32_t lastProcessedInputSequence = 0;
        std::string actorName;

        glm::ivec3 lastChunkPos{INT_MAX, INT_MAX, INT_MAX};
        std::unordered_set<glm::ivec3> cachedVisibleChunks;
        std::unordered_set<glm::ivec3> coreChunks;

        std::unordered_map<glm::ivec3, NetChunkUpdate> pendingChunkUpdates;

        flatbuffers::FlatBufferBuilder entitySnapshotBuilder{8192};
        flatbuffers::FlatBufferBuilder chunkUpdateBuilder{ChunkData::MAX_SERIALIZED_SIZE + 256};
    };

    Session& getOrCreateSession(uint32_t sessionId);
    NetEntitySnapshot buildEntitySnapshot(Session& session);
    void sendChunkUpdates(Session& session);
    void updateChunks();
    void updateSessionChunkDemand(Session& session, glm::ivec3 currentChunkPos, ServerChunkManager::DemandMap& demands);
    void processQueuedChunks();
    void processPendingUnloads(ServerChunkManager::TimePoint now);
    bool commitChunkLoad(glm::ivec3 chunkPos, ChunkData&& data, uint64_t generationId);
    bool commitChunkUnload(glm::ivec3 chunkPos);
    void queueChunkUpdate(Session& session, NetChunkUpdate update);

    NetChunkUpdate buildUpsertChunkUpdate(const Chunk& chunk);
    static NetChunkUpdate buildUnloadChunkUpdate(glm::ivec3 chunkPos, uint32_t revision);
    void pumpNetwork();

    void onSessionConnect(uint32_t sessionId);
    void onSessionDisconnect(uint32_t sessionId);
    bool onSessionPacket(uint32_t sessionId, const std::vector<uint8_t>& packet);
    bool onClientHello(uint32_t sessionId);
    void onClientReady(uint32_t sessionId);
    void onClientInput(uint32_t sessionId, const NetClientInput& input);

    ServerChunkManager chunkManager_{std::chrono::seconds(3)};
    VoxelWorld voxelWorld_;
    ActorWorld actorWorld_{true};
    std::vector<std::unique_ptr<System>> systems_;

    asio::io_context ioContext_;

    std::unique_ptr<INetServer> netServer_;
    std::unordered_map<uint32_t, Session> sessions_;
    uint32_t nextPlayerIndex_ = 1;
};
