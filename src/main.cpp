#include <iostream>

#include "entity.h"
#include "world.h"

int main(int argc, char* argv[]) {
    std::cout << "========================================" << std::endl;
    std::cout << "  MineCraft-Style Game (EnTT Edition)" << std::endl;
    std::cout << "  ECS-based Game Engine" << std::endl;
    std::cout << "========================================" << std::endl;

    World world;

    world.initializeDefaultWorld();

    auto player1 = world.createPlayer("Steve", glm::vec3(7.5f, 4.0f, 7.5f));
    auto player2 = world.createPlayer("Alex", glm::vec3(3.0f, 4.0f, 3.0f));
    world.getRegistry().get<Velocity>(player1) = glm::vec3(1.0f, 0.0f, 0.5f);
    world.getRegistry().get<Velocity>(player2) = glm::vec3(-0.5f, 0.0f, 0.0f);

    float deltaTime = 0.016f;
    int totalFrames = 30;
    for (int frame = 0; frame < totalFrames; ++frame) {
        world.update(deltaTime);
    }

    return 0;
}