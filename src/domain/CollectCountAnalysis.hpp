// src/domain/CollectCountAnalysis.hpp
#pragma once

#include "domain/ResourceSample.hpp"

#include <expected>

namespace monitor {

struct DailyCountWindow {
    std::uint32_t current_day{};
    std::uint32_t previous_day{};

    bool operator==(const DailyCountWindow&) const = default;
};

struct DailyCountPair {
    std::string eqp_ip;
    std::uint64_t current_count{};
    std::uint64_t previous_count{};
    bool current_present{};
    bool previous_present{};
};

enum class CollectCountReadStatus { pending, ready, error };

struct CollectCountReading {
    CollectCountReadStatus status{CollectCountReadStatus::pending};
    std::optional<std::chrono::system_clock::time_point> queried_at;
    std::vector<DailyCountPair> counts;
    std::optional<Error> error;
};

enum class CollectCountStatus { disabled, pending, ok, warning, no_data, error };

struct CollectCountAlert {
    std::string eqp_ip;
    std::uint64_t current_count{};
    std::uint64_t previous_count{};
    bool current_present{};
    bool previous_present{};
    std::optional<double> change_percent;
    std::string reason;
};

struct CollectCountAnalysis {
    CollectCountStatus status{CollectCountStatus::disabled};
    DailyCountWindow window;
    double threshold_percent{10.0};
    std::optional<std::chrono::system_clock::time_point> queried_at;
    std::size_t equipment_count{};
    std::vector<CollectCountAlert> alerts;
    std::optional<Error> error;
};

std::expected<CollectCountAnalysis, Error> analyze_collect_counts(
    const std::vector<DailyCountPair>& counts, double threshold_percent);
const char* to_string(CollectCountStatus status) noexcept;

}
