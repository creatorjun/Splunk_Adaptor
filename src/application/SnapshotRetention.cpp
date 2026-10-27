// src/application/SnapshotRetention.cpp
#include "application/SnapshotRetention.hpp"

#include <exception>

namespace monitor {

SnapshotRetention::SnapshotRetention(ISnapshotRetention& storage) noexcept : storage_(storage) {}

std::expected<std::size_t, Error> SnapshotRetention::run(std::chrono::system_clock::time_point now,
    const SnapshotRetentionSettings& settings) {
    if (!settings.enabled) return std::size_t{0};
    if (settings.max_age < SnapshotRetentionSettings::minimum_age ||
        settings.max_age > SnapshotRetentionSettings::maximum_age) {
        return std::unexpected(Error{"snapshot_retention", "settings", "invalid_retention",
            "Retention max_age_seconds must be between 60 and 31536000"});
    }
    if (now < std::chrono::system_clock::time_point::min() + settings.max_age) {
        return std::unexpected(Error{"snapshot_retention", "clock", "invalid_timestamp",
            "Snapshot retention cutoff is outside the supported time range"});
    }
    try {
        return storage_.prune(now - settings.max_age);
    } catch (const std::exception&) {
        return std::unexpected(Error{"snapshot_retention", "output", "cleanup_failed",
            "Snapshot retention operation failed"});
    }
}

}
