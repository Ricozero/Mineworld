#include <chrono>
#include <memory>
#include <string_view>
#include <thread>

#include "block.h"
#include "entity.h"
#include "game_client.h"
#include "game_server.h"
#include "log.h"
#include "render_context.h"

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
    server.loadChunk(glm::ivec3(0, 0, 0));

    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            server.setBlock(glm::ivec3(x, 0, z), BlockData{BlockType::Grass, BlockOrientation::Up});
            if ((x + z) % 5 == 0) {
                server.setBlock(glm::ivec3(x, 1, z), BlockData{BlockType::Dirt, BlockOrientation::North});
            }
        }
    }

    for (int y = 1; y < 5; ++y) {
        server.setBlock(glm::ivec3(3, y, 3), BlockData{BlockType::Wood, BlockOrientation::Up});
    }
    server.setBlock(glm::ivec3(2, 5, 3), BlockData{BlockType::Leaves, BlockOrientation::North});
    server.setBlock(glm::ivec3(3, 5, 3), BlockData{BlockType::Leaves, BlockOrientation::North});
    server.setBlock(glm::ivec3(4, 5, 3), BlockData{BlockType::Leaves, BlockOrientation::North});
    server.setBlock(glm::ivec3(3, 5, 2), BlockData{BlockType::Leaves, BlockOrientation::North});
    server.setBlock(glm::ivec3(3, 5, 4), BlockData{BlockType::Leaves, BlockOrientation::North});
    server.setBlock(glm::ivec3(10, 1, 10), BlockData{BlockType::Stone, BlockOrientation::North});
    server.setBlock(glm::ivec3(11, 1, 10), BlockData{BlockType::Stone, BlockOrientation::North});
    server.setBlock(glm::ivec3(10, 2, 10), BlockData{BlockType::Stone, BlockOrientation::North});

    auto steve = server.world().createPlayer("Steve", glm::vec3(8.0f, 1.0f, 8.0f));
    auto alex = server.world().createPlayer("Alex", glm::vec3(12.0f, 1.0f, 12.0f));
    auto& registry = server.world().getActorWorld().registry();
    registry.get<PhysicsComponent>(steve).useGravity = false;
    registry.get<PhysicsComponent>(alex).useGravity = false;
}

}  // namespace

int main(int argc, char* argv[]) {
    logging::init();

    const RunMode runMode = parseRunMode(argc, argv);

    std::unique_ptr<GameServer> server;
    std::unique_ptr<GameClient> client;
    std::unique_ptr<RenderContext> renderContext;

    if (runMode == RunMode::Combined || runMode == RunMode::ServerOnly) {
        server = std::make_unique<GameServer>();
        seedServerWorld(*server);
    }

    if (runMode == RunMode::Combined || runMode == RunMode::ClientOnly) {
        renderContext = std::make_unique<RenderContext>();
        if (!renderContext->initialize(1280, 720, "Mineworld")) {
            return 1;
        }
        client = std::make_unique<GameClient>(renderContext.get());
    }

    if (!renderContext) {
        int fps = 60;
        float deltaTime = 1.0f / fps;
        int runSeconds = 3;
        for (int frame = 0; frame < fps * runSeconds; ++frame) {
            if (server) {
                server->update(deltaTime);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        logging::info("Simulation completed");
        return 0;
    }

    auto previousTime = std::chrono::steady_clock::now();
    while (!renderContext->shouldClose()) {
        const auto currentTime = std::chrono::steady_clock::now();
        const std::chrono::duration<float> elapsed = currentTime - previousTime;
        previousTime = currentTime;
        const float deltaTime = elapsed.count();

        renderContext->pollEvents();
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
