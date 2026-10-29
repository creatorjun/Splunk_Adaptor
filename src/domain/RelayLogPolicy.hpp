// src/domain/RelayLogPolicy.hpp
#pragma once

#include "domain/RelayLog.hpp"

#include <cstddef>

namespace monitor {

enum class RelayLogLevel { other, error };

class RelayLogSelection {
public:
    static bool accepts_level(RelayLogLevel level) noexcept;
    void consume(char value) noexcept;
    bool matches(RelayLogLevel level) const noexcept;
    void reset() noexcept;

private:
    std::size_t matched_{};
    bool found_{};
};

RelayLogStatus evaluate_relay_log(bool has_events, bool has_errors, bool backlog) noexcept;

}
