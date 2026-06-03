#include <chrono>
#include <memory>
#include <string_view>

#include "entity.h"
#include "game_client.h"
#include "game_server.h"
#include "log.h"
#include "profiler.h"
#include "render_context.h"

namespace {

constexpr PlayerMode kLocalEntryMode = PlayerMode::Spectator;

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

bool initializeServer(std::unique_ptr<GameServer>& server) {
    logging::Scope logScope(logging::Channel::Server);
    server = std::make_unique<GameServer>(kLocalEntryMode);

    // Create robots (AI-controlled entities)
    auto steve = server->createRobot("Steve", glm::vec3(8.0f, 1.0f, 8.0f));
    auto alice = server->createRobot("Alice", glm::vec3(12.0f, 1.0f, 12.0f));

    // Disable gravity for robots so they stay on the surface
    auto& registry = server->world().getActorWorld().registry();
    registry.get<PhysicsComponent>(steve).useGravity = false;
    registry.get<PhysicsComponent>(alice).useGravity = false;

    return true;
}

bool initializeClient(std::unique_ptr<GameClient>& client, std::unique_ptr<RenderContext>& renderContext) {
    logging::Scope logScope(logging::Channel::Client);
    renderContext = std::make_unique<RenderContext>();
    if (!renderContext->initialize(1280, 720, "Mineworld")) {
        return false;
    }
    client = std::make_unique<GameClient>(renderContext.get());
    return true;
}

int runServerOnly() {
    profiling::Profiler::instance().setThreadName("ServerMain");

    std::unique_ptr<GameServer> server;
    if (!initializeServer(server)) {
        return 1;
    }
    auto previousTime = std::chrono::steady_clock::now();

    logging::info("Dedicated server started");
    for (;;) {
        MW_PROFILE_SCOPE("Frame.Total");

        const auto currentTime = std::chrono::steady_clock::now();
        const std::chrono::duration<float> elapsed = currentTime - previousTime;
        previousTime = currentTime;
        server->update(elapsed.count());
    }
}

int runClientOnly() {
    profiling::Profiler::instance().setThreadName("ClientMain");

    std::unique_ptr<GameClient> client;
    std::unique_ptr<RenderContext> renderContext;
    if (!initializeClient(client, renderContext)) {
        return 1;
    }

    auto previousTime = std::chrono::steady_clock::now();
    while (!renderContext->shouldClose()) {
        MW_PROFILE_SCOPE("Frame.Total");

        const auto currentTime = std::chrono::steady_clock::now();
        const std::chrono::duration<float> elapsed = currentTime - previousTime;
        previousTime = currentTime;

        renderContext->pollEvents();
        client->update(elapsed.count());
    }
    return 0;
}

int runCombined() {
    profiling::Profiler::instance().setThreadName("Main");

    std::unique_ptr<GameServer> server;
    if (!initializeServer(server)) {
        return 1;
    }

    std::unique_ptr<GameClient> client;
    std::unique_ptr<RenderContext> renderContext;
    if (!initializeClient(client, renderContext)) {
        return 1;
    }

    auto previousTime = std::chrono::steady_clock::now();
    while (!renderContext->shouldClose()) {
        MW_PROFILE_SCOPE("Frame.Total");

        const auto currentTime = std::chrono::steady_clock::now();
        const std::chrono::duration<float> elapsed = currentTime - previousTime;
        previousTime = currentTime;

        renderContext->pollEvents();
        server->update(elapsed.count());
        client->update(elapsed.count());
    }
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    logging::init();

    const RunMode runMode = parseRunMode(argc, argv);

    switch (runMode) {
        case RunMode::ClientOnly:
            return runClientOnly();
        case RunMode::ServerOnly:
            return runServerOnly();
        case RunMode::Combined:
            return runCombined();
    }

    return 0;
}
