#pragma once

#include <charconv>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>
#include <string>
#include <string_view>
#include <unordered_map>

// Minimal INI parser. Supports [sections], key = value, and # / ; comments.
// Keys are stored as "section.key". Sections before the first header are in "".
class Config {
public:
    bool load(const std::filesystem::path& path) {
        std::ifstream file(path);
        if (!file) {
            return false;
        }
        std::string section;
        std::string line;
        while (std::getline(file, line)) {
            parseLine(line, section);
        }
        return true;
    }

    std::string get(std::string_view key, std::string_view fallback = "") const {
        auto it = entries_.find(std::string(key));
        return it != entries_.end() ? it->second : std::string(fallback);
    }

    int getInt(std::string_view key, int fallback = 0) const {
        auto it = entries_.find(std::string(key));
        if (it == entries_.end()) {
            return fallback;
        }
        int value = fallback;
        std::from_chars(it->second.data(), it->second.data() + it->second.size(), value);
        return value;
    }

    bool getBool(std::string_view key, bool fallback = false) const {
        auto it = entries_.find(std::string(key));
        if (it == entries_.end()) {
            return fallback;
        }
        const std::string& v = it->second;
        if (v == "true" || v == "1" || v == "yes") return true;
        if (v == "false" || v == "0" || v == "no") return false;
        return fallback;
    }

    float getFloat(std::string_view key, float fallback = 0.0f) const {
        auto it = entries_.find(std::string(key));
        if (it == entries_.end()) {
            return fallback;
        }
        try {
            return std::stof(it->second);
        } catch (...) {
            return fallback;
        }
    }

private:
    void parseLine(std::string_view line, std::string& section) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            return;
        }
        if (line[0] == '[') {
            const auto close = line.find(']');
            if (close != std::string_view::npos) {
                section = std::string(line.substr(1, close - 1));
            }
            return;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            return;
        }
        std::string key = std::string(trim(line.substr(0, eq)));
        std::string value = std::string(trim(line.substr(eq + 1)));
        for (char delim : {'#', ';'}) {
            const auto pos = value.find(delim);
            if (pos != std::string::npos) {
                value = std::string(trim(value.substr(0, pos)));
            }
        }
        const std::string fullKey = section.empty() ? key : section + "." + key;
        entries_[fullKey] = value;
    }

    static std::string_view trim(std::string_view s) {
        const auto first = s.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) {
            return {};
        }
        const auto last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    }

    std::unordered_map<std::string, std::string> entries_;
};

struct AppConfig {
    // [window]
    int windowWidth = 1280;
    int windowHeight = 720;

    // [render]
    std::string graphicsApi = "dx11";

    // [server]
    uint16_t port = 40000;
    int ticksPerSecond = 20;
    int chunkViewRadius = 2;

    // [spawn]
    glm::vec3 spawnPosition{0.0f, 2.0f, 0.0f};
    float spawnYaw = -90.0f;
    float spawnPitch = -12.0f;

    // [physics]
    float gravity = 9.8f;
    float collisionEpsilon = 0.001f;
    float groundProbeDistance = 0.05f;
    float jumpSpeed = 5.0f;
    float maxFallSpeed = 50.0f;
    float survivalSprintMultiplier = 1.6f;
    float spectatorSprintMultiplier = 5.0f;

    static AppConfig& instance() {
        static AppConfig inst;
        return inst;
    }

    void load(const std::string& configDir) {
        Config cfg;
        cfg.load(configDir + "config.ini");
        windowWidth = cfg.getInt("window.width", windowWidth);
        windowHeight = cfg.getInt("window.height", windowHeight);
        graphicsApi = cfg.get("render.graphics_api", graphicsApi);
        port = static_cast<uint16_t>(cfg.getInt("server.port", port));
        ticksPerSecond = cfg.getInt("server.ticks_per_second", ticksPerSecond);
        chunkViewRadius = cfg.getInt("server.chunk_view_radius", chunkViewRadius);
        spawnPosition.x = cfg.getFloat("spawn.x", spawnPosition.x);
        spawnPosition.y = cfg.getFloat("spawn.y", spawnPosition.y);
        spawnPosition.z = cfg.getFloat("spawn.z", spawnPosition.z);
        spawnYaw = cfg.getFloat("spawn.yaw", spawnYaw);
        spawnPitch = cfg.getFloat("spawn.pitch", spawnPitch);
        gravity = cfg.getFloat("physics.gravity", gravity);
        collisionEpsilon = cfg.getFloat("physics.collision_epsilon", collisionEpsilon);
        groundProbeDistance = cfg.getFloat("physics.ground_probe_distance", groundProbeDistance);
        jumpSpeed = cfg.getFloat("physics.jump_speed", jumpSpeed);
        maxFallSpeed = cfg.getFloat("physics.max_fall_speed", maxFallSpeed);
        survivalSprintMultiplier = cfg.getFloat("physics.survival_sprint_multiplier", survivalSprintMultiplier);
        spectatorSprintMultiplier = cfg.getFloat("physics.spectator_sprint_multiplier", spectatorSprintMultiplier);
    }

private:
    AppConfig() = default;
};
