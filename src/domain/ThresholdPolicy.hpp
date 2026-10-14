// src/domain/ThresholdPolicy.hpp
#pragma once

#include "domain/ResourceSample.hpp"

#include <expected>

namespace monitor {

struct ThresholdPolicy {
    double cpu_usage_percent{};
    double memory_usage_percent{};
    double storage_usage_percent{};

    bool operator==(const ThresholdPolicy&) const = default;
};

enum class MetricStatus { ok, warning, unknown };
enum class CollectionStatus { ok, partial, error };
enum class ConfigStatus { ok, using_last_valid };

std::expected<void, Error> validate_thresholds(const ThresholdPolicy& policy);
MetricStatus evaluate_threshold(double usage_percent, double threshold);
const char* to_string(MetricStatus status) noexcept;
const char* to_string(CollectionStatus status) noexcept;
const char* to_string(ConfigStatus status) noexcept;

}
