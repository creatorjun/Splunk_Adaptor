// src/domain/RelayLog.hpp
#pragma once

#include "domain/ResourceSample.hpp"

namespace monitor {

enum class RelayLogStatus { disabled, ok, warning, error, partial };

struct RelayLogEntry {
    std::string timestamp;
    std::string message;
    std::string line;
    bool truncated{false};
};

struct RelayLogSnapshot {
    RelayLogStatus status{RelayLogStatus::disabled};
    std::string path;
    std::optional<std::chrono::system_clock::time_point> checked_at;
    std::vector<RelayLogEntry> events;
    bool backlog{false};
    std::vector<Error> errors;
};

const char* to_string(RelayLogStatus status) noexcept;

}
