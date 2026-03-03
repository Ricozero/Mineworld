#include <spdlog/spdlog.h>

#include "game_client.h"
#include "game_server.h"

int main(int argc, char* argv[]) {
    spdlog::set_pattern("%+");

    GameServer server;
    GameClient client;

    server.world().createPlayer("Steve");
    server.world().createPlayer("Alex", glm::vec3(100.0f, 100.0f, 100.0f));
    server.world().createPlayer("Steve");

    client.world().createPlayer("Steve");
    client.world().createPlayer("Alex", glm::vec3(100.0f, 100.0f, 100.0f));

    float deltaTime = 0.016f;
    int totalFrames = 120;
    for (int frame = 0; frame < totalFrames; ++frame) {
        server.update(deltaTime);
        client.update(deltaTime);
    }
    return 0;
}
