// src/domain/MetricCalculators.hpp
#pragma once

#include "domain/ResourceSnapshot.hpp"

#include <expected>

namespace monitor {

std::expected<CpuMetrics, Error> calculate_cpu(const CpuCounters& previous,
    const CpuCounters& current, double window_seconds, double threshold);
std::expected<MemoryMetrics, Error> calculate_memory(const MemoryCounters& counters,
    double threshold);
std::expected<FilesystemMetrics, Error> calculate_filesystem(const FilesystemSample& sample,
    double threshold);
void summarize(ResourceSnapshot& snapshot, bool filesystem_enumeration_failed);

}
