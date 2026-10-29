// src/application/ports/ILogCountStore.hpp
#pragma once

#include "domain/LogVolume.hpp"

#include <expected>

namespace monitor {

class ILogCountStore {
public:
    virtual ~ILogCountStore() = default;
    virtual std::expected<std::optional<LogCountState>, Error> load(const std::string& directory) = 0;
    virtual std::expected<void, Error> commit(const std::string& directory, const LogCountState& state,
        const std::vector<LogCountReport>& reports) = 0;
    virtual std::expected<std::size_t, Error> prune(const std::string& directory,
        std::chrono::system_clock::time_point cutoff) = 0;
};

}