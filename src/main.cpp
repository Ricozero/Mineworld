#include <memory>
#include <string_view>

#include "block.h"
#include "game_client.h"
#include "game_server.h"
#include "log.h"

namespace {

enum class RunMode {
    Combined,
    ClientOnly,
    ServerOnly,
};

RunMode parseRunMode(int argc, char* argv[]) {
    if (argc < 2) {
        return RunMode::Combined;
    }

    const std::string_view arg = argv[1];
    if (arg == "client") {
        return RunMode::ClientOnly;
    }
    if (arg == "server") {
        return RunMode::ServerOnly;
    }
    if (arg == "combined") {
        return RunMode::Combined;
    }

    logging::warn("Unknown run mode '{}', defaulting to combined", arg);
    return RunMode::Combined;
}

void seedServerWorld(GameServer& server) {
    logging::Scope logScope(logging::Channel::Server);
    server.world().createPlayer("Steve");
    server.world().createPlayer("Alex", glm::vec3(100.0f, 100.0f, 100.0f));
    server.loadChunk(glm::ivec3(0, 0, 0));
    server.setBlock(glm::ivec3(1, 2, 3), BlockData{BlockType::Stone, BlockOrientation::North});
}

}  // namespace

int main(int argc, char* argv[]) {
    logging::init();

    const RunMode runMode = parseRunMode(argc, argv);

    std::unique_ptr<GameServer> server;
    std::unique_ptr<GameClient> client;

    if (runMode == RunMode::Combined || runMode == RunMode::ServerOnly) {
        server = std::make_unique<GameServer>();
        seedServerWorld(*server);
    }

    if (runMode == RunMode::Combined || runMode == RunMode::ClientOnly) {
        client = std::make_unique<GameClient>();
    }

    int fps = 60;
    float deltaTime = 1.0f / fps;
    int runSeconds = 3;
    for (int frame = 0; frame < fps * runSeconds; ++frame) {
        if (server) {
            server->update(deltaTime);
        }
        if (client) {
            client->update(deltaTime);
        }
    }
    logging::info("Simulation completed");
    return 0;
}
