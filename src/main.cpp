#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "config.h"
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

    server->createRobot("Steve", glm::vec3(8.0f, 1.0f, 8.0f));
    server->createRobot("Alice", glm::vec3(12.0f, 1.0f, 12.0f));

    return true;
}

bool initializeRenderContext(std::unique_ptr<RenderContext>& renderContext) {
    logging::Scope logScope(logging::Channel::Client);
    renderContext = std::make_unique<RenderContext>();
    const AppConfig& cfg = AppConfig::instance();
    if (!renderContext->initialize(cfg.windowWidth, cfg.windowHeight, "Mineworld")) {
        return false;
    }
    return true;
}

int runServer(const std::string& dir) {
    profiling::Profiler::instance().setThreadName("ServerMain");

    AppConfig::instance().load(dir);
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
    const auto kTickInterval = std::chrono::microseconds(1'000'000 / AppConfig::instance().ticksPerSecond);
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

int runClient(const std::string& dir) {
    profiling::Profiler::instance().setThreadName("ClientMain");

    AppConfig::instance().load(dir);
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
    int port = AppConfig::instance().port;
    std::string connectingAddress;
    uint16_t connectingPort = AppConfig::instance().port;

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
                    if (playMode == ClientPlayMode::Local) {
                        connectingAddress = "127.0.0.1";
                        connectingPort = AppConfig::instance().port;
                        if (!initializeServer(localServer)) {
                            return 1;
                        }
                        serverThread = std::thread(runLocalServer, localServer.get(), std::ref(stopServer));
                    } else {
                        connectingAddress = addressBuffer;
                        connectingPort = static_cast<uint16_t>(std::clamp(port, 1, 65535));
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

    std::filesystem::path path(argv[0]);
    const std::string dir = path.has_parent_path() ? path.parent_path().string() + "/" : "./";
    const RunMode runMode = parseRunMode(argc, argv);

    switch (runMode) {
        case RunMode::Client:
            return runClient(dir);
        case RunMode::Server:
            return runServer(dir);
    }

    return 0;
}
