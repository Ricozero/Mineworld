#pragma once

#include <memory>
#include <optional>
#include <string>

struct StdinLine {
    std::string text;
    std::string error;
};

class StdinConsole {
public:
    StdinConsole();
    ~StdinConsole();
    StdinConsole(const StdinConsole&) = delete;
    StdinConsole& operator=(const StdinConsole&) = delete;

    void poll();
    std::optional<StdinLine> consumeLine();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
