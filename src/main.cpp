#include <chrono>
#include <memory>
#include <string_view>
#include <thread>

#include "entity.h"
#include "game_client.h"
#include "game_server.h"
#include "log.h"
#include "profiler.h"
#include "render_context.h"

namespace {

constexpr const char* kSpectatorName = "Spectator";

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

std::unique_ptr<GameServer> createServer() {
    auto server = std::make_unique<GameServer>();
    auto steve = server->createPlayer("Steve", glm::vec3(8.0f, 1.0f, 8.0f));
    auto alex = server->createPlayer("Alex", glm::vec3(12.0f, 1.0f, 12.0f));
    auto spectator = server->createSpectator(kSpectatorName, glm::vec3(8.0f, 6.0f, 24.0f));
    auto& registry = server->world().getActorWorld().registry();
    registry.get<PhysicsComponent>(steve).useGravity = false;
    registry.get<PhysicsComponent>(alex).useGravity = false;
    registry.get<PhysicsComponent>(spectator).useGravity = false;
    return server;
}

bool initializeClient(std::unique_ptr<RenderContext>& renderContext,
                      std::unique_ptr<GameClient>& client) {
    renderContext = std::make_unique<RenderContext>();
    if (!renderContext->initialize(1280, 720, "Mineworld")) {
        return false;
    }
    client = std::make_unique<GameClient>(renderContext.get(), kSpectatorName);
    return true;
}

void syncCameraToServer(GameServer& server, RenderContext& renderContext) {
    // Directly update the server's spectator entity position from camera
    glm::vec3 camPos = renderContext.getCameraPosition();
    auto& registry = server.world().getActorWorld().registry();
    auto view = registry.view<SpectatorComponent, TransformComponent>();
    for (auto entity : view) {
        auto& transform = registry.get<TransformComponent>(entity);
        transform.position = camPos;
        server.world().getActorWorld().updateEntityChunk(entity, camPos);
    }
}

void updateServer(GameServer& server, float deltaTime) {
    profiling::ScopedTimer timer("App.ServerUpdate");
    server.update(deltaTime);
}

void updateClient(GameClient& client, float deltaTime) {
    profiling::ScopedTimer timer("App.ClientUpdate");
    client.update(deltaTime);
}

int runServerOnly() {
    std::unique_ptr<GameServer> server = createServer();
    auto previousTime = std::chrono::steady_clock::now();

    logging::info("Dedicated server started");
    for (;;) {
        profiling::ScopedTimer frameTimer("Frame.Total");

        const auto currentTime = std::chrono::steady_clock::now();
        const std::chrono::duration<float> elapsed = currentTime - previousTime;
        previousTime = currentTime;
        updateServer(*server, elapsed.count());

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

int runClientOnly() {
    std::unique_ptr<GameClient> client;
    std::unique_ptr<RenderContext> renderContext;
    if (!initializeClient(renderContext, client)) {
        return 1;
    }

    auto previousTime = std::chrono::steady_clock::now();
    while (!renderContext->shouldClose()) {
        profiling::ScopedTimer frameTimer("Frame.Total");

        const auto currentTime = std::chrono::steady_clock::now();
        const std::chrono::duration<float> elapsed = currentTime - previousTime;
        previousTime = currentTime;
        const float deltaTime = elapsed.count();

        {
            profiling::ScopedTimer timer("App.PollEvents");
            renderContext->pollEvents();
        }
        updateClient(*client, deltaTime);
    }

    logging::info("Client stopped");
    return 0;
}

int runCombined() {
    std::unique_ptr<GameServer> server = createServer();
    std::unique_ptr<GameClient> client;
    std::unique_ptr<RenderContext> renderContext;
    if (!initializeClient(renderContext, client)) {
        return 1;
    }

    auto previousTime = std::chrono::steady_clock::now();
    while (!renderContext->shouldClose()) {
        profiling::ScopedTimer frameTimer("Frame.Total");

        const auto currentTime = std::chrono::steady_clock::now();
        const std::chrono::duration<float> elapsed = currentTime - previousTime;
        previousTime = currentTime;
        const float deltaTime = elapsed.count();

        {
            profiling::ScopedTimer timer("App.PollEvents");
            renderContext->pollEvents();
        }

        syncCameraToServer(*server, *renderContext);

        updateServer(*server, deltaTime);
        updateClient(*client, deltaTime);
    }

    logging::info("Combined mode stopped");
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