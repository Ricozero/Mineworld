#include <spdlog/spdlog.h>

#include "block.h"
#include "game_client.h"
#include "game_server.h"

int main(int argc, char* argv[]) {
    spdlog::set_pattern("%+");

    GameServer server;
    GameClient client;

    server.world().createPlayer("Steve");
    server.world().createPlayer("Alex", glm::vec3(100.0f, 100.0f, 100.0f));
    server.loadChunk(glm::ivec3(0, 0, 0));
    server.setBlock(glm::ivec3(1, 2, 3), BlockData{BlockType::Stone, BlockOrientation::North});

    float deltaTime = 0.016f;
    int totalFrames = 120;
    for (int frame = 0; frame < totalFrames; ++frame) {
        server.update(deltaTime);
        client.update(deltaTime);
    }
    spdlog::info("Simulation completed");
    return 0;
}
