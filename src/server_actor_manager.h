#pragma once

#include <vector>

#include "actor_world.h"
#include "net_protocol.h"

class ServerActorManager {
public:
    explicit ServerActorManager(ActorWorld& world);

    ServerActorManager(const ServerActorManager&) = delete;
    ServerActorManager& operator=(const ServerActorManager&) = delete;

    ActorId allocateId();

    void collectSnapshot();
    void selectSnapshot(glm::vec3 center, float radius, std::vector<const NetActorState*>& selected) const;

private:
    ActorWorld& world_;
    ActorId nextId_ = 1;
    std::vector<NetActorState> snapshotStates_;
};
