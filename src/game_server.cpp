#include "game_server.h"

#include "system.h"

GameServer::GameServer() {
    registerSystem(std::make_unique<PhysicsSystem>());
}

GameServer::~GameServer() = default;

void GameServer::registerSystem(std::unique_ptr<BaseSystem> system) {
    systems_.push_back(std::move(system));
}

void GameServer::update(float deltaTime) {
    for (auto& system : systems_) {
        system->update(world_, deltaTime);
    }
}
