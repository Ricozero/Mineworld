#include "world.h"

int main(int argc, char* argv[]) {
    World world;

    world.createPlayer("Steve");
    world.createPlayer("Alex", glm::vec3(100.0f, 100.0f, 100.0f));
    world.createPlayer("Steve");

    float deltaTime = 0.016f;
    int totalFrames = 120;
    for (int frame = 0; frame < totalFrames; ++frame) {
        world.update(deltaTime);
    }
    return 0;
}