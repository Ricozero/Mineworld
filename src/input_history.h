#pragma once

#include <cstddef>
#include <string>
#include <vector>

class InputHistory {
public:
    const std::string& current() const;
    bool isDraft() const { return position_ == entries_.size(); }
    void previous();
    void next();
    void edit(std::string text);
    void submit();

private:
    static constexpr size_t MAX_ENTRIES = 100;
    std::vector<std::string> entries_;
    std::string draft_;
    size_t position_ = 0;
};
