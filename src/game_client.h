#pragma once

#include <memory>
#include <vector>

#include "world.h"

class BaseSystem;

class GameClient {
public:
    GameClient();
    ~GameClient();

    World& world() { return world_; }
    const World& world() const { return world_; }

    void registerSystem(std::unique_ptr<BaseSystem> system);
    void update(float deltaTime);

private:
    World world_;
    std::vector<std::unique_ptr<BaseSystem>> systems_;
};
