// src/application/CollectAndStore.cpp
#include "application/CollectAndStore.hpp"

#include "domain/MetricCalculators.hpp"

#include <utility>

namespace monitor {

CollectAndStore::CollectAndStore(IResourceReader& reader, ISnapshotStore& store,
    IThresholdConfigReader& config, ICollectCountMonitor* collect_count,
    IJavaProcessMonitor* java_processes)
    : reader_(reader), store_(store), config_(config), collect_count_(collect_count),
      java_processes_(java_processes) {
}

std::expected<void, Error> CollectAndStore::initialize() {
    policy_.reset();
    previous_cpu_.reset();
    auto configured = config_.read();
    if (!configured) {
        return std::unexpected(configured.error());
    }
    if (auto validation = validate_thresholds(*configured); !validation) {
        return std::unexpected(validation.error());
    }
    auto sample = reader_.read();
    policy_ = *configured;
    policy_applied_at_ = sample.timestamp;
    if (collect_count_) collect_count_->collect(sample.timestamp);
    if (sample.cpu.value && !sample.cpu.error) {
        previous_cpu_ = *sample.cpu.value;
        previous_cpu_time_ = sample.cpu_observed_at;
    }
    return {};
}

void CollectAndStore::settings_changed(std::chrono::system_clock::time_point timestamp) {
    if (policy_ && collect_count_) collect_count_->settings_changed(timestamp);
}

std::expected<ResourceSnapshot, Error> CollectAndStore::run(std::uint64_t missed_intervals) {
    if (!policy_) {
        return std::unexpected(Error{"application", "", "not_initialized",
            "Collection requires a valid initial threshold configuration"});
    }
    ResourceSnapshot snapshot;
    bool policy_changed{};
    auto configured = config_.read();
    if (!configured) {
        snapshot.config_status = ConfigStatus::using_last_valid;
        snapshot.errors.push_back(configured.error());
    } else if (auto validation = validate_thresholds(*configured); !validation) {
        snapshot.config_status = ConfigStatus::using_last_valid;
        snapshot.errors.push_back(validation.error());
    } else if (*configured != *policy_) {
        policy_ = *configured;
        policy_changed = true;
    }
    if (const auto reload_error = config_.reload_error()) {
        snapshot.config_status = ConfigStatus::using_last_valid;
        snapshot.errors.push_back(*reload_error);
    }
    auto sample = reader_.read();
    if (policy_changed) {
        policy_applied_at_ = sample.timestamp;
    }
    snapshot.timestamp = sample.timestamp;
    snapshot.thresholds_applied_at = policy_applied_at_;
    snapshot.hostname = std::move(sample.hostname);
    snapshot.applied_thresholds = *policy_;
    snapshot.missed_intervals = missed_intervals;

    if (sample.cpu.error) {
        snapshot.errors.push_back(*sample.cpu.error);
        previous_cpu_.reset();
    } else if (!sample.cpu.value) {
        snapshot.errors.push_back(Error{"cpu", "", "missing_reading", "CPU counters are unavailable"});
        previous_cpu_.reset();
    } else {
        if (previous_cpu_) {
            const double seconds = std::chrono::duration<double>(
                sample.cpu_observed_at - previous_cpu_time_).count();
            auto calculated = calculate_cpu(*previous_cpu_, *sample.cpu.value,
                seconds, policy_->cpu_usage_percent);
            if (calculated) {
                snapshot.cpu = *calculated;
            } else {
                snapshot.errors.push_back(calculated.error());
            }
        } else {
            snapshot.errors.push_back(Error{"cpu", "", "insufficient_baseline",
                "CPU usage requires two valid consecutive observations"});
        }
        previous_cpu_ = *sample.cpu.value;
        previous_cpu_time_ = sample.cpu_observed_at;
    }

    if (sample.memory.error) {
        snapshot.errors.push_back(*sample.memory.error);
    } else if (!sample.memory.value) {
        snapshot.errors.push_back(Error{"memory", "", "missing_reading",
            "Memory counters are unavailable"});
    } else {
        auto calculated = calculate_memory(*sample.memory.value, policy_->memory_usage_percent);
        if (calculated) {
            snapshot.memory = *calculated;
        } else {
            snapshot.errors.push_back(calculated.error());
        }
    }

    if (sample.filesystem_enumeration_error) {
        snapshot.errors.push_back(*sample.filesystem_enumeration_error);
    }
    if (sample.filesystems.empty() && !sample.filesystem_enumeration_error) {
        snapshot.errors.push_back(Error{"storage", "", "no_filesystems",
            "No monitored filesystems were discovered"});
    }
    snapshot.filesystems.reserve(sample.filesystems.size());
    for (const auto& filesystem : sample.filesystems) {
        auto calculated = calculate_filesystem(filesystem, policy_->storage_usage_percent);
        if (calculated) {
            if (!calculated->usage_percent) {
                snapshot.errors.push_back(Error{"storage", filesystem.device, "zero_usable_capacity",
                    "Filesystem has no used or user-available blocks for a usage percentage"});
            }
            snapshot.filesystems.push_back(std::move(*calculated));
        } else {
            FilesystemMetrics unavailable;
            unavailable.device = filesystem.device;
            unavailable.device_id = filesystem.device_id;
            unavailable.filesystem_type = filesystem.filesystem_type;
            unavailable.mount_points = filesystem.mount_points;
            snapshot.filesystems.push_back(std::move(unavailable));
            snapshot.errors.push_back(calculated.error());
        }
    }
    summarize(snapshot, sample.filesystem_enumeration_error.has_value());
    if (collect_count_) {
        snapshot.collect_count = collect_count_->collect(snapshot.timestamp);
        if (snapshot.collect_count.error) snapshot.errors.push_back(*snapshot.collect_count.error);
        if (snapshot.collect_count.status == CollectCountStatus::warning) {
            snapshot.status = MetricStatus::warning;
        } else if (snapshot.collect_count.status != CollectCountStatus::ok &&
            snapshot.collect_count.status != CollectCountStatus::disabled && snapshot.status == MetricStatus::ok) {
            snapshot.status = MetricStatus::unknown;
        }
    }
    if (java_processes_) {
        snapshot.java_processes = java_processes_->collect();
        snapshot.errors.insert(snapshot.errors.end(), snapshot.java_processes.errors.begin(),
            snapshot.java_processes.errors.end());
        if (snapshot.java_processes.status == MetricStatus::warning) {
            snapshot.status = MetricStatus::warning;
        } else if (snapshot.java_processes.status == MetricStatus::unknown &&
            snapshot.status == MetricStatus::ok) {
            snapshot.status = MetricStatus::unknown;
        }
    }
    if (auto saved = store_.save(snapshot); !saved) {
        auto error = saved.error();
        for (const auto& cycle_error : snapshot.errors) {
            error.message += " | cycle_error{component=" + cycle_error.component
                + " target=" + cycle_error.target + " code=" + cycle_error.code
                + " message=" + cycle_error.message + "}";
        }
        return std::unexpected(std::move(error));
    }
    return snapshot;
}

}
