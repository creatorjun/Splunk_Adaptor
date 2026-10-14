// src/domain/ThresholdPolicy.cpp
#include "domain/ThresholdPolicy.hpp"

#include <cmath>
#include <string>

namespace monitor {

std::expected<void, Error> validate_thresholds(const ThresholdPolicy& policy) {
    const auto validate = [](double value, const char* name) -> std::expected<void, Error> {
        if (!std::isfinite(value) || value < 0.0 || value > 100.0) {
            return std::unexpected(Error{"thresholds", name, "invalid_threshold",
                "Threshold must be a finite number between 0 and 100 inclusive"});
        }
        return {};
    };
    if (auto result = validate(policy.cpu_usage_percent, "cpu_usage_percent"); !result) {
        return result;
    }
    if (auto result = validate(policy.memory_usage_percent, "memory_usage_percent"); !result) {
        return result;
    }
    return validate(policy.storage_usage_percent, "storage_usage_percent");
}

MetricStatus evaluate_threshold(double usage_percent, double threshold) {
    if (!std::isfinite(usage_percent) || !std::isfinite(threshold)
        || usage_percent < 0.0 || usage_percent > 100.0
        || threshold < 0.0 || threshold > 100.0) {
        return MetricStatus::unknown;
    }
    return usage_percent >= threshold ? MetricStatus::warning : MetricStatus::ok;
}

const char* to_string(MetricStatus status) noexcept {
    switch (status) {
    case MetricStatus::ok: return "ok";
    case MetricStatus::warning: return "warning";
    case MetricStatus::unknown: return "unknown";
    }
    return "unknown";
}

const char* to_string(CollectionStatus status) noexcept {
    switch (status) {
    case CollectionStatus::ok: return "ok";
    case CollectionStatus::partial: return "partial";
    case CollectionStatus::error: return "error";
    }
    return "error";
}

const char* to_string(ConfigStatus status) noexcept {
    switch (status) {
    case ConfigStatus::ok: return "ok";
    case ConfigStatus::using_last_valid: return "using_last_valid";
    }
    return "using_last_valid";
}

}
