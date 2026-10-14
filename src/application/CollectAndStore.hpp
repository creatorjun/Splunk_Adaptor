// src/application/CollectAndStore.hpp
#pragma once

#include "application/ports/IResourceReader.hpp"
#include "application/ports/ISnapshotStore.hpp"
#include "application/ports/IThresholdConfigReader.hpp"

namespace monitor {

class CollectAndStore {
public:
    CollectAndStore(IResourceReader& reader, ISnapshotStore& store,
        IThresholdConfigReader& config);
    std::expected<void, Error> initialize();
    std::expected<ResourceSnapshot, Error> run(std::uint64_t missed_intervals = 0);

private:
    IResourceReader& reader_;
    ISnapshotStore& store_;
    IThresholdConfigReader& config_;
    std::optional<ThresholdPolicy> policy_;
    std::chrono::system_clock::time_point policy_applied_at_;
    std::optional<CpuCounters> previous_cpu_;
    std::chrono::steady_clock::time_point previous_cpu_time_;
};

}
