// src/application/CollectAndStore.hpp
#pragma once

#include "application/ports/IResourceReader.hpp"
#include "application/ports/ISnapshotStore.hpp"
#include "application/ports/IThresholdConfigReader.hpp"
#include "application/ports/ICollectCountMonitor.hpp"
#include "application/ports/IJavaProcessMonitor.hpp"
#include "application/ports/ILogSilenceMonitor.hpp"

namespace monitor {

class CollectAndStore {
public:
    CollectAndStore(IResourceReader& reader, ISnapshotStore& store,
        IThresholdConfigReader& config, ICollectCountMonitor* collect_count = nullptr,
        IJavaProcessMonitor* java_processes = nullptr,
        ILogSilenceMonitor* log_silence = nullptr);
    std::expected<void, Error> initialize();
    void settings_changed(std::chrono::system_clock::time_point timestamp);
    std::expected<ResourceSnapshot, Error> run(std::uint64_t missed_intervals = 0);

private:
    IResourceReader& reader_;
    ISnapshotStore& store_;
    IThresholdConfigReader& config_;
    ICollectCountMonitor* collect_count_;
    IJavaProcessMonitor* java_processes_;
    ILogSilenceMonitor* log_silence_;
    std::optional<ThresholdPolicy> policy_;
    std::chrono::system_clock::time_point policy_applied_at_;
    std::optional<CpuCounters> previous_cpu_;
    std::chrono::steady_clock::time_point previous_cpu_time_;
};

}
