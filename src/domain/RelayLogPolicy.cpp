// src/domain/RelayLogPolicy.cpp
#include "domain/RelayLogPolicy.hpp"

#include <array>
#include <string_view>

namespace monitor {

bool RelayLogSelection::accepts_level(RelayLogLevel level) noexcept {
    return level == RelayLogLevel::error;
}

void RelayLogSelection::consume(char value) noexcept {
    if (found_) return;
    constexpr std::string_view keyword{"connection"};
    constexpr std::array<std::size_t, 10> prefix{0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
    const char lowered = value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
    while (matched_ > 0 && lowered != keyword[matched_]) matched_ = prefix[matched_ - 1];
    if (lowered == keyword[matched_]) ++matched_;
    found_ = matched_ == keyword.size();
}

bool RelayLogSelection::matches(RelayLogLevel level) const noexcept {
    return accepts_level(level) && found_;
}

void RelayLogSelection::reset() noexcept {
    matched_ = 0;
    found_ = false;
}

RelayLogStatus evaluate_relay_log(bool has_events, bool has_errors, bool backlog) noexcept {
    return has_events ? RelayLogStatus::warning
        : has_errors ? RelayLogStatus::error
        : backlog ? RelayLogStatus::partial : RelayLogStatus::ok;
}

}
