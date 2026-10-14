// src/domain/ResourceSnapshot.hpp
#pragma once

#include "domain/ResourceSample.hpp"
#include "domain/ThresholdPolicy.hpp"

namespace monitor {

struct CpuMetrics {
    MetricStatus status{MetricStatus::unknown};
    std::optional<double> window_seconds;
    std::optional<double> usage_percent;
    std::optional<double> iowait_percent;
    std::optional<double> steal_percent;
};

struct MemoryMetrics {
    MetricStatus status{MetricStatus::unknown};
    std::optional<std::uint64_t> total_bytes;
    std::optional<std::uint64_t> used_bytes;
    std::optional<std::uint64_t> available_bytes;
    std::optional<double> usage_percent;
};

struct FilesystemMetrics {
    MetricStatus status{MetricStatus::unknown};
    std::string device;
    std::string device_id;
    std::string filesystem_type;
    std::vector<std::string> mount_points;
    std::optional<std::uint64_t> total_bytes;
    std::optional<std::uint64_t> used_bytes;
    std::optional<std::uint64_t> free_bytes;
    std::optional<std::uint64_t> available_bytes;
    std::optional<double> usage_percent;
};

struct ResourceSnapshot {
    std::chrono::system_clock::time_point timestamp;
    std::chrono::system_clock::time_point thresholds_applied_at;
    std::string hostname;
    MetricStatus status{MetricStatus::unknown};
    CollectionStatus collection_status{CollectionStatus::error};
    ConfigStatus config_status{ConfigStatus::ok};
    ThresholdPolicy applied_thresholds;
    std::uint64_t missed_intervals{};
    CpuMetrics cpu;
    MemoryMetrics memory;
    std::vector<FilesystemMetrics> filesystems;
    std::vector<Error> errors;
};

}
