#include "input_history.h"

#include "text.h"

const std::string& InputHistory::current() const {
    return position_ < entries_.size() ? entries_[position_] : draft_;
}

void InputHistory::previous() {
    if (position_ > 0) --position_;
}

void InputHistory::next() {
    if (position_ < entries_.size()) ++position_;
    else draft_.clear();
}

void InputHistory::edit(std::string text) {
    draft_ = std::move(text);
    position_ = entries_.size();
}

void InputHistory::submit() {
    std::string text = current();
    if (!trimText(text).empty()) {
        if (entries_.size() == MAX_ENTRIES) entries_.erase(entries_.begin());
        entries_.push_back(std::move(text));
    }
    draft_.clear();
    position_ = entries_.size();
}
