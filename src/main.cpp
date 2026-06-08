#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "entity.h"
#include "game_client.h"
#include "game_server.h"
#include "log.h"
#include "profiler.h"
#include "render_context.h"

namespace {

enum class RunMode {
    Client,
    Server,
};

enum class ClientState {
    StartMenu,
    Connecting,
    InGame,
};

enum class ClientPlayMode {
    Remote,
    Local,
};

constexpr uint16_t kDefaultServerPort = 40000;
constexpr const char* kDefaultServerAddress = "127.0.0.1";

RunMode parseRunMode(int argc, char* argv[]) {
    if (argc < 2) {
        return RunMode::Client;
    }

    const std::string_view arg = argv[1];
    if (arg == "client") {
        return RunMode::Client;
    }
    if (arg == "server") {
        return RunMode::Server;
    }

    logging::warn("Unknown run mode '{}', defaulting to client", arg);
    return RunMode::Client;
}

bool initializeServer(std::unique_ptr<GameServer>& server) {
    logging::Scope logScope(logging::Channel::Server);
    server = std::make_unique<GameServer>();

    // Create robots (AI-controlled entities)
    auto steve = server->createRobot("Steve", glm::vec3(8.0f, 1.0f, 8.0f));
    auto alice = server->createRobot("Alice", glm::vec3(12.0f, 1.0f, 12.0f));

    // Disable gravity for robots so they stay on the surface
    auto& registry = server->world().getActorWorld().registry();
    registry.get<PhysicsComponent>(steve).useGravity = false;
    registry.get<PhysicsComponent>(alice).useGravity = false;

    return true;
}

bool initializeRenderContext(std::unique_ptr<RenderContext>& renderContext) {
    logging::Scope logScope(logging::Channel::Client);
    renderContext = std::make_unique<RenderContext>();
    if (!renderContext->initialize(1280, 720, "Mineworld")) {
        return false;
    }
    return true;
}

int runServer() {
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

void stopClientSession(std::unique_ptr<GameClient>& client, std::unique_ptr<GameServer>& localServer,
                       std::thread& serverThread, std::atomic<bool>& stopServer) {
    if (client) {
        client->disconnect();
        client.reset();
    }
    if (localServer) {
        stopServer = true;
        if (serverThread.joinable()) {
            serverThread.join();
        }
        localServer.reset();
    }
}

void runLocalServer(GameServer* server, std::atomic<bool>& stopServer) {
    profiling::Profiler::instance().setThreadName("LocalServer");
    constexpr auto kTickInterval = std::chrono::milliseconds(50);  // 20 ticks/s
    auto nextTick = std::chrono::steady_clock::now();
    auto previousTime = nextTick;
    while (!stopServer) {
        std::this_thread::sleep_until(nextTick);
        nextTick += kTickInterval;
        const auto currentTime = std::chrono::steady_clock::now();
        const std::chrono::duration<float> elapsed = currentTime - previousTime;
        previousTime = currentTime;
        server->update(elapsed.count());
    }
}

int runClient() {
    profiling::Profiler::instance().setThreadName("ClientMain");

    std::unique_ptr<RenderContext> renderContext;
    if (!initializeRenderContext(renderContext)) {
        return 1;
    }

    ClientState state = ClientState::StartMenu;
    ClientPlayMode playMode = ClientPlayMode::Remote;
    std::unique_ptr<GameClient> client;
    std::unique_ptr<GameServer> localServer;
    std::thread serverThread;
    std::atomic<bool> stopServer{false};
    char addressBuffer[128] = "127.0.0.1";
    int port = kDefaultServerPort;
    std::string connectingAddress = kDefaultServerAddress;
    uint16_t connectingPort = kDefaultServerPort;

    auto previousTime = std::chrono::steady_clock::now();
    while (!renderContext->shouldClose()) {
        MW_PROFILE_SCOPE("Frame.Total");

        const auto currentTime = std::chrono::steady_clock::now();
        const std::chrono::duration<float> elapsed = currentTime - previousTime;
        previousTime = currentTime;

        renderContext->pollEvents();

        switch (state) {
            case ClientState::StartMenu: {
                const RenderContext::StartMenuAction action = renderContext->renderStartMenu(addressBuffer, sizeof(addressBuffer), port);
                if (action == RenderContext::StartMenuAction::Quit) {
                    return 0;
                }
                if (action == RenderContext::StartMenuAction::Local || action == RenderContext::StartMenuAction::Remote) {
                    stopClientSession(client, localServer, serverThread, stopServer);
                    stopServer = false;
                    playMode = action == RenderContext::StartMenuAction::Local ? ClientPlayMode::Local : ClientPlayMode::Remote;
                    connectingAddress = playMode == ClientPlayMode::Local ? kDefaultServerAddress : addressBuffer;
                    connectingPort = static_cast<uint16_t>(std::clamp(port, 1, 65535));
                    if (playMode == ClientPlayMode::Local) {
                        if (!initializeServer(localServer)) {
                            return 1;
                        }
                        serverThread = std::thread(runLocalServer, localServer.get(), std::ref(stopServer));
                    }
                    client = std::make_unique<GameClient>(renderContext.get(), connectingAddress, connectingPort);
                    renderContext->captureMouse();
                    state = ClientState::Connecting;
                }
                break;
            }
            case ClientState::Connecting:
                if (client) {
                    client->update(elapsed.count());
                    if (client->isSessionReady()) {
                        renderContext->closeInGameMenu();
                        state = ClientState::InGame;
                    } else {
                        const RenderContext::ConnectingAction action = renderContext->renderConnecting(connectingAddress, connectingPort);
                        if (action == RenderContext::ConnectingAction::Cancel) {
                            stopClientSession(client, localServer, serverThread, stopServer);
                            stopServer = false;
                            renderContext->releaseMouse();
                            state = ClientState::StartMenu;
                        }
                    }
                }
                break;
            case ClientState::InGame:
                if (client) {
                    client->update(elapsed.count());
                }
                if (renderContext->consumeInGameMenuAction() == RenderContext::InGameMenuAction::ReturnToStart) {
                    stopClientSession(client, localServer, serverThread, stopServer);
                    stopServer = false;
                    renderContext->releaseMouse();
                    state = ClientState::StartMenu;
                }
                break;
        }
    }
    stopClientSession(client, localServer, serverThread, stopServer);
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    logging::init();

    const RunMode runMode = parseRunMode(argc, argv);

    switch (runMode) {
        case RunMode::Client:
            return runClient();
        case RunMode::Server:
            return runServer();
    }

    return 0;
}
