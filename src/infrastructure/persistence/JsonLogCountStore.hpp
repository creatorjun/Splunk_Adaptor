// src/infrastructure/persistence/JsonLogCountStore.hpp
#pragma once

#include "application/ports/ILogCountStore.hpp"
#include "infrastructure/linux/UniqueFd.hpp"

namespace monitor {

class JsonLogCountStore final : public ILogCountStore {
public:
    std::expected<std::optional<LogCountState>, Error> load(const std::string& directory) override;
    std::expected<void, Error> commit(const std::string& directory, const LogCountState& state,
        const std::vector<LogCountReport>& reports) override;
    std::expected<std::size_t, Error> prune(const std::string& directory,
        std::chrono::system_clock::time_point cutoff) override;

private:
    std::expected<void, Error> open_directory(const std::string& directory);
    UniqueFd directory_fd_;
    UniqueFd lock_fd_;
    std::string directory_;
};

}