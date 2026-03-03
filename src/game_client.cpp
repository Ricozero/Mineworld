#include "game_client.h"

#include "system.h"

GameClient::GameClient() {
    registerSystem(std::make_unique<InputSystem>());
    registerSystem(std::make_unique<RenderSystem>());
}

GameClient::~GameClient() = default;

void GameClient::registerSystem(std::unique_ptr<BaseSystem> system) {
    systems_.push_back(std::move(system));
}

void GameClient::update(float deltaTime) {
    for (auto& system : systems_) {
        system->update(world_, deltaTime);
    }
}
