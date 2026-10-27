// src/application/SnapshotRetention.cpp
#include "application/SnapshotRetention.hpp"

#include <exception>

namespace monitor {

SnapshotRetention::SnapshotRetention(ISnapshotRetention& storage) noexcept : storage_(storage) {}

std::expected<std::size_t, Error> SnapshotRetention::run(std::chrono::system_clock::time_point now) {
    if (now < std::chrono::system_clock::time_point::min() + maximum_age) {
        return std::unexpected(Error{"snapshot_retention", "clock", "invalid_timestamp",
            "Snapshot retention cutoff is outside the supported time range"});
    }
    try {
        return storage_.prune(now - maximum_age);
    } catch (const std::exception&) {
        return std::unexpected(Error{"snapshot_retention", "output", "cleanup_failed",
            "Snapshot retention operation failed"});
    }
}

}
