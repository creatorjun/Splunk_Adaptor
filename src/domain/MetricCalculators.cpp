// src/domain/MetricCalculators.cpp
#include "domain/MetricCalculators.hpp"

#include <cmath>
#include <limits>

namespace monitor {
namespace {

bool valid_threshold(double threshold) {
    return std::isfinite(threshold) && threshold >= 0.0 && threshold <= 100.0;
}

double percentage(std::uint64_t numerator, std::uint64_t denominator) {
    return static_cast<double>(static_cast<long double>(numerator) * 100.0L
        / static_cast<long double>(denominator));
}

}

std::expected<CpuMetrics, Error> calculate_cpu(const CpuCounters& previous,
    const CpuCounters& current, double window_seconds, double threshold) {
    if (!valid_threshold(threshold)) {
        return std::unexpected(Error{"cpu", "threshold", "invalid_threshold",
            "CPU threshold must be between 0 and 100 inclusive"});
    }
    if (!std::isfinite(window_seconds) || window_seconds <= 0.0) {
        return std::unexpected(Error{"cpu", "", "invalid_window",
            "CPU observation window must be finite and positive"});
    }
    std::array<std::uint64_t, 8> deltas{};
    std::uint64_t total{};
    for (std::size_t index = 0; index < deltas.size(); ++index) {
        if (current.ticks[index] < previous.ticks[index]) {
            return std::unexpected(Error{"cpu", std::to_string(index), "counter_regression",
                "CPU counter decreased; a new observation baseline is required"});
        }
        deltas[index] = current.ticks[index] - previous.ticks[index];
        if (deltas[index] > std::numeric_limits<std::uint64_t>::max() - total) {
            return std::unexpected(Error{"cpu", "", "counter_overflow",
                "CPU interval total exceeds the supported counter range"});
        }
        total += deltas[index];
    }
    if (total == 0) {
        return std::unexpected(Error{"cpu", "", "insufficient_cpu_delta",
            "CPU counters did not advance during the observation window"});
    }
    const std::uint64_t busy = deltas[0] + deltas[1] + deltas[2] + deltas[5] + deltas[6];
    CpuMetrics metrics;
    metrics.window_seconds = window_seconds;
    metrics.usage_percent = percentage(busy, total);
    metrics.iowait_percent = percentage(deltas[4], total);
    metrics.steal_percent = percentage(deltas[7], total);
    metrics.status = evaluate_threshold(*metrics.usage_percent, threshold);
    return metrics;
}

std::expected<MemoryMetrics, Error> calculate_memory(const MemoryCounters& counters,
    double threshold) {
    if (!valid_threshold(threshold)) {
        return std::unexpected(Error{"memory", "threshold", "invalid_threshold",
            "Memory threshold must be between 0 and 100 inclusive"});
    }
    if (counters.total_bytes == 0 || counters.available_bytes > counters.total_bytes) {
        return std::unexpected(Error{"memory", "", "invalid_counters",
            "Memory total must be positive and available memory must not exceed total"});
    }
    MemoryMetrics metrics;
    metrics.total_bytes = counters.total_bytes;
    metrics.available_bytes = counters.available_bytes;
    metrics.used_bytes = counters.total_bytes - counters.available_bytes;
    metrics.usage_percent = percentage(*metrics.used_bytes, *metrics.total_bytes);
    metrics.status = evaluate_threshold(*metrics.usage_percent, threshold);
    return metrics;
}

std::expected<FilesystemMetrics, Error> calculate_filesystem(const FilesystemSample& sample,
    double threshold) {
    if (!valid_threshold(threshold)) {
        return std::unexpected(Error{"storage", sample.device, "invalid_threshold",
            "Storage threshold must be between 0 and 100 inclusive"});
    }
    if (sample.counters.error) {
        return std::unexpected(*sample.counters.error);
    }
    if (!sample.counters.value) {
        return std::unexpected(Error{"storage", sample.device, "missing_reading",
            "Filesystem counters are unavailable"});
    }
    const auto& counters = *sample.counters.value;
    if (counters.fragment_size == 0
        || counters.free_blocks > counters.blocks
        || counters.available_blocks > counters.free_blocks) {
        return std::unexpected(Error{"storage", sample.device, "invalid_counters",
            "Filesystem capacity and block counters are inconsistent"});
    }
    if (counters.blocks > std::numeric_limits<std::uint64_t>::max() / counters.fragment_size) {
        return std::unexpected(Error{"storage", sample.device, "counter_overflow",
            "Filesystem byte capacity exceeds the supported counter range"});
    }
    const auto used_blocks = counters.blocks - counters.free_blocks;
    const auto usable_blocks = used_blocks + counters.available_blocks;
    FilesystemMetrics metrics;
    metrics.device = sample.device;
    metrics.device_id = sample.device_id;
    metrics.filesystem_type = sample.filesystem_type;
    metrics.mount_points = sample.mount_points;
    metrics.total_bytes = counters.blocks * counters.fragment_size;
    metrics.used_bytes = used_blocks * counters.fragment_size;
    metrics.free_bytes = counters.free_blocks * counters.fragment_size;
    metrics.available_bytes = counters.available_blocks * counters.fragment_size;
    if (usable_blocks == 0) {
        return metrics;
    }
    metrics.usage_percent = percentage(used_blocks, usable_blocks);
    metrics.status = evaluate_threshold(*metrics.usage_percent, threshold);
    return metrics;
}

void summarize(ResourceSnapshot& snapshot, bool filesystem_enumeration_failed) {
    std::size_t successful{};
    std::size_t failed{};
    bool warning{};
    const auto include = [&](MetricStatus status) {
        if (status == MetricStatus::unknown) {
            ++failed;
        } else {
            ++successful;
            warning = warning || status == MetricStatus::warning;
        }
    };
    include(snapshot.cpu.status);
    include(snapshot.memory.status);
    for (const auto& filesystem : snapshot.filesystems) {
        include(filesystem.status);
    }
    if (filesystem_enumeration_failed || snapshot.filesystems.empty()) {
        ++failed;
    }
    snapshot.status = warning ? MetricStatus::warning
        : failed > 0 ? MetricStatus::unknown : MetricStatus::ok;
    snapshot.collection_status = successful == 0 ? CollectionStatus::error
        : failed > 0 ? CollectionStatus::partial : CollectionStatus::ok;
}

}
