// src/application/SnapshotRetention.hpp
#pragma once

#include "application/ports/ISnapshotRetention.hpp"
#include "application/SnapshotRetentionSettings.hpp"

namespace monitor {

class SnapshotRetention {
public:
    explicit SnapshotRetention(ISnapshotRetention& storage) noexcept;
    std::expected<std::size_t, Error> run(std::chrono::system_clock::time_point now,
        const SnapshotRetentionSettings& settings = {});
    static constexpr std::chrono::seconds check_interval{60};

private:
    ISnapshotRetention& storage_;
};

}
