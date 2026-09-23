#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__linux__)
#include <system_error>
#endif

#include "config.h"
#include "game_client.h"
#include "game_server.h"
#include "log.h"
#include "profiler.h"
#include "render_context.h"
#include "stdin_console.h"
#include "text.h"

namespace {

std::filesystem::path executablePath(const char* argv0) {
#if defined(_WIN32)
    std::wstring buffer(256, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) break;
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__linux__)
    std::error_code error;
    const std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) return path;
#endif

    std::error_code error;
    const std::filesystem::path path = argv0 ? argv0 : "";
    const std::filesystem::path absolutePath = std::filesystem::absolute(path, error);
    return error ? path : absolutePath;
}

enum class RunMode {
    Client,
    HeadlessClient,
    Server,
};

enum class ClientState {
    StartMenu,
    ConnectionScreen,
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
    if (arg == "headless-client") {
        return RunMode::HeadlessClient;
    }
    if (arg == "server") {
        return RunMode::Server;
    }

    logging::warn("Unknown run mode '{}', defaulting to client", arg);
    return RunMode::Client;
}

struct Options {
    std::string address;
    uint16_t port = 0;
    bool integratedServer = false;
};

std::optional<Options> parseOptions(int argc, char* argv[]) {
    const auto& cfg = AppConfig::instance();
    Options options{cfg.serverAddress, cfg.port};
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--integrated-server") {
            options.integratedServer = true;
            continue;
        }
        if (arg != "--address" && arg != "--port") {
            logging::error("Unknown option '{}'", arg);
            return std::nullopt;
        }
        if (i + 1 == argc || std::string_view(argv[i + 1]).starts_with("--")) {
            logging::error("Missing value for '{}'", arg);
            return std::nullopt;
        }
        const std::string_view value = argv[++i];
        if (arg == "--address") {
            options.address = value;
        } else if (arg == "--port") {
            unsigned port = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), port);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || port == 0 || port > 65535) {
                logging::error("Invalid port '{}': expected an integer from 1 to 65535", value);
                return std::nullopt;
            }
            options.port = static_cast<uint16_t>(port);
        }
    }
    if (options.integratedServer) {
        options.address = "127.0.0.1";
    }
    asio::error_code addressError;
    const auto address = asio::ip::make_address(options.address, addressError);
    if (addressError || !address.is_v4()) {
        logging::error("Invalid server address '{}': expected an IPv4 address", options.address);
        return std::nullopt;
    }
    if (options.port == 0) {
        logging::error("Invalid server port: expected an integer from 1 to 65535");
        return std::nullopt;
    }
    return options;
}

std::atomic<bool> stopRequested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void handleSignal(int) {
    stopRequested.store(true, std::memory_order_relaxed);
}

void initializeSignalHandlers() {
    stopRequested = false;
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
}

bool initializeServer(std::unique_ptr<GameServer>& server) {
    try {
        server = std::make_unique<GameServer>();
        return true;
    } catch (const std::exception& error) {
        logging::error("Failed to start server on port {}: {}", AppConfig::instance().port, error.what());
        return false;
    }
}

bool initializeRenderContext(std::unique_ptr<RenderContext>& renderContext, const std::string& dir) {
    renderContext = std::make_unique<RenderContext>();
    const AppConfig& cfg = AppConfig::instance();
    if (!renderContext->initialize(cfg.windowWidth, cfg.windowHeight, "Mineworld", dir)) {
        return false;
    }
    return true;
}

void stopClientSession(std::unique_ptr<GameClient>& client, std::unique_ptr<GameServer>& localServer, std::thread& serverThread, std::atomic<bool>& stopServer) {
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
    logging::setThreadChannel(logging::Channel::Server);
    const auto kTickInterval = std::chrono::microseconds(1'000'000 / AppConfig::instance().ticksPerSecond);
    auto nextTick = std::chrono::steady_clock::now();
    auto previousTime = nextTick;

    logging::info("Local server started");
    while (!stopServer) {
        std::this_thread::sleep_until(nextTick);
        nextTick += kTickInterval;
        const auto currentTime = std::chrono::steady_clock::now();
        const std::chrono::duration<float> elapsed = currentTime - previousTime;
        previousTime = currentTime;
        server->update(elapsed.count());
    }
}

void displayCommandResponse(const CommandResponse& response, RenderContext* renderContext = nullptr) {
    if (renderContext) renderContext->appendConsoleLine(response.message, response.success);
    else if (response.success) logging::info("{}", response.message);
    else logging::warn("{}", response.message);
}

void displayClientMessages(GameClient& client, RenderContext* renderContext = nullptr) {
    while (auto message = client.consumeMessage()) {
        if (const auto* response = std::get_if<CommandResponse>(&*message)) displayCommandResponse(*response, renderContext);
        else {
            const auto& chat = std::get<ChatMessage>(*message);
            if (renderContext) renderContext->appendConsoleLine(chat.name + ": " + chat.text);
            else logging::info("{}: {}", chat.name, chat.text);
        }
    }
}

bool isConsoleExit(std::string_view text) {
    text = trimText(text);
    if (text.starts_with('/')) text.remove_prefix(1);
    return text == "quit" || text == "exit" || text == "stop";
}

int runClient(const std::string& dir) {
    profiling::Profiler::instance().setThreadName("ClientMain");
    logging::setThreadChannel(logging::Channel::Client);

    AppConfig::instance().load(dir);
    std::unique_ptr<RenderContext> renderContext;
    if (!initializeRenderContext(renderContext, dir)) {
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
        const auto currentTime = std::chrono::steady_clock::now();
        const std::chrono::duration<float> elapsed = currentTime - previousTime;
        previousTime = currentTime;

        renderContext->pollEvents();
        renderContext->beginFrame();

        switch (state) {
            case ClientState::StartMenu: {
                const RenderContext::StartMenuAction action = renderContext->renderStartMenu(addressBuffer, sizeof(addressBuffer), port);
                if (action == RenderContext::StartMenuAction::Quit) {
                    return 0;
                }
                if (action == RenderContext::StartMenuAction::Local || action == RenderContext::StartMenuAction::Remote) {
                    stopClientSession(client, localServer, serverThread, stopServer);
                    renderContext->resetInGameMenu();
                    stopServer = false;
                    playMode = action == RenderContext::StartMenuAction::Local ? ClientPlayMode::Local : ClientPlayMode::Remote;
                    if (playMode == ClientPlayMode::Local) {
                        connectingAddress = "127.0.0.1";
                        connectingPort = AppConfig::instance().port;
                        if (!initializeServer(localServer)) {
                            break;
                        }
                        serverThread = std::thread(runLocalServer, localServer.get(), std::ref(stopServer));
                    } else {
                        connectingAddress = addressBuffer;
                        connectingPort = static_cast<uint16_t>(std::clamp(port, 1, 65535));
                    }
                    client = std::make_unique<GameClient>(renderContext.get(), connectingAddress, connectingPort);
                    state = ClientState::ConnectionScreen;
                }
                break;
            }
            case ClientState::ConnectionScreen:
                if (client) {
                    client->update(elapsed.count());
                    if (client->isSessionReady()) {
                        renderContext->setGameActive(true);
                        renderContext->captureMouse();
                        state = ClientState::InGame;
                    } else {
                        const RenderContext::ConnectingAction action = renderContext->renderConnecting(connectingAddress, connectingPort, client->statusText(), client->hasFailed());
                        if (action == RenderContext::ConnectingAction::Cancel) {
                            stopClientSession(client, localServer, serverThread, stopServer);
                            renderContext->resetInGameMenu();
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
                    if (client->hasFailed()) {
                        renderContext->resetInGameMenu();
                        renderContext->releaseMouse();
                        state = ClientState::ConnectionScreen;
                        break;
                    }
                }
                if (renderContext->consumeInGameMenuAction() == RenderContext::InGameMenuAction::ReturnToStart) {
                    stopClientSession(client, localServer, serverThread, stopServer);
                    renderContext->resetInGameMenu();
                    stopServer = false;
                    renderContext->releaseMouse();
                    state = ClientState::StartMenu;
                }
                break;
        }
        if (client) {
            while (auto text = renderContext->consumeConsoleInput()) client->submitText(*text);
            displayClientMessages(*client, renderContext.get());
        }
        renderContext->endFrame();
        const auto frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - currentTime).count();
        profiling::Profiler::instance().finishFrame(frameMs);
    }
    stopClientSession(client, localServer, serverThread, stopServer);
    return 0;
}

int runHeadlessClient(const std::string& dir, int argc, char* argv[]) {
    profiling::Profiler::instance().setThreadName("HeadlessClient");
    logging::setThreadChannel(logging::Channel::Client);

    auto& cfg = AppConfig::instance();
    cfg.load(dir);
    const auto options = parseOptions(argc, argv);
    if (!options) {
        logging::info("Usage: mineworld headless-client [--address IPv4] [--port 1-65535] [--integrated-server]");
        return 1;
    }
    initializeSignalHandlers();
    StdinConsole console;

    std::unique_ptr<GameClient> client;
    std::unique_ptr<GameServer> localServer;
    std::thread serverThread;
    std::atomic<bool> stopServer{false};
    bool localServerFailed = false;
    int exitCode = 0;
    try {
        if (options->integratedServer) {
            cfg.port = options->port;
            if (!initializeServer(localServer)) {
                return 1;
            }
        }
        client = std::make_unique<GameClient>(nullptr, options->address, options->port);
        if (localServer) {
            serverThread = std::thread([&] {
                try {
                    runLocalServer(localServer.get(), stopServer);
                } catch (const std::exception& error) {
                    logging::error("Local server failed: {}", error.what());
                    localServerFailed = true;
                    stopServer = true;
                }
            });
        }

        logging::info("Headless client connecting to {}:{} (Ctrl+C to stop)", options->address, options->port);
        constexpr auto kUpdateInterval = std::chrono::milliseconds(10);
        auto previousTime = std::chrono::steady_clock::now();
        while (!stopRequested.load(std::memory_order_relaxed) && !stopServer) {
            const auto currentTime = std::chrono::steady_clock::now();
            const std::chrono::duration<float> elapsed = currentTime - previousTime;
            previousTime = currentTime;
            client->update(elapsed.count());
            console.poll();
            while (auto line = console.consumeLine()) {
                if (!line->error.empty()) displayCommandResponse({0, false, std::move(line->error)});
                else if (isConsoleExit(line->text)) {
                    stopRequested = true;
                    break;
                } else client->submitCommand(line->text);
            }
            displayClientMessages(*client);
            const auto frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - currentTime).count();
            profiling::Profiler::instance().finishFrame(frameMs);
            if (client->hasFailed()) {
                exitCode = 1;
                break;
            }
            std::this_thread::sleep_until(currentTime + kUpdateInterval);
        }
    } catch (const std::exception& error) {
        logging::error("Headless client failed: {}", error.what());
        exitCode = 1;
    }

    stopClientSession(client, localServer, serverThread, stopServer);
    logging::info("Headless client stopped");
    return localServerFailed ? 1 : exitCode;
}

int runServer(const std::string& dir, int argc, char* argv[]) {
    profiling::Profiler::instance().setThreadName("ServerMain");
    logging::setThreadChannel(logging::Channel::Server);

    auto& cfg = AppConfig::instance();
    cfg.load(dir);
    const auto options = parseOptions(argc, argv);
    if (!options) {
        logging::info("Usage: mineworld server [--port 1-65535]");
        return 1;
    }
    initializeSignalHandlers();
    StdinConsole console;
    cfg.port = options->port;
    std::unique_ptr<GameServer> server;
    if (!initializeServer(server)) {
        return 1;
    }
    auto previousTime = std::chrono::steady_clock::now();

    logging::info("Dedicated server started on port {} (Ctrl+C to stop)", options->port);
    while (!stopRequested.load(std::memory_order_relaxed)) {
        const auto currentTime = std::chrono::steady_clock::now();
        const std::chrono::duration<float> elapsed = currentTime - previousTime;
        previousTime = currentTime;
        console.poll();
        while (auto line = console.consumeLine()) {
            if (!line->error.empty()) displayCommandResponse({0, false, std::move(line->error)});
            else if (isConsoleExit(line->text)) {
                stopRequested = true;
                break;
            } else if (!trimText(line->text).empty()) displayCommandResponse(server->executeConsoleCommand(line->text));
        }
        server->update(elapsed.count());
        const auto frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - currentTime).count();
        profiling::Profiler::instance().finishFrame(frameMs);
    }
    server.reset();
    logging::info("Dedicated server stopped");
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::filesystem::path path = executablePath(argc > 0 ? argv[0] : nullptr);
    const std::string dir = path.has_parent_path() ? path.parent_path().string() + "/" : "./";
    logging::init(dir);

    const RunMode runMode = parseRunMode(argc, argv);

    switch (runMode) {
        case RunMode::Client:
            return runClient(dir);
        case RunMode::HeadlessClient:
            return runHeadlessClient(dir, argc, argv);
        case RunMode::Server:
            return runServer(dir, argc, argv);
    }

    return 0;
}
