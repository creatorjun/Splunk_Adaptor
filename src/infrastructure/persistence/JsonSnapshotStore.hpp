// src/infrastructure/persistence/JsonSnapshotStore.hpp
#pragma once

#include "application/ports/ISnapshotStore.hpp"
#include "application/ports/IOutputDirectory.hpp"
#include "application/ports/ISnapshotRetention.hpp"

#include <filesystem>
#include <mutex>

namespace monitor {

class JsonSnapshotStore final : public ISnapshotStore, public IOutputDirectory, public ISnapshotRetention {
public:
    explicit JsonSnapshotStore(std::filesystem::path directory);
    ~JsonSnapshotStore() override;
    std::expected<void, Error> configure_output(const std::string& directory) override;
    std::expected<void, Error> save(const ResourceSnapshot& snapshot) override;
    std::expected<std::size_t, Error> prune(std::chrono::system_clock::time_point cutoff) override;

private:
    std::expected<void, Error> configure_output_locked(std::filesystem::path directory);

    std::mutex mutex_;
    std::filesystem::path directory_;
    int directory_fd_{-1};
    int lock_fd_{-1};
};

}
