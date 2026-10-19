// src/domain/CollectCountAnalysis.cpp
#include "domain/CollectCountAnalysis.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace monitor {

std::expected<CollectCountAnalysis, Error> analyze_collect_counts(
    const std::vector<DailyCountPair>& counts, double threshold_percent) {
    if (!std::isfinite(threshold_percent) || threshold_percent <= 0.0
        || threshold_percent > 1000000.0) {
        return std::unexpected(Error{"collect_count", "threshold", "invalid_threshold",
            "Collection count threshold must be finite, positive, and at most 1000000"});
    }
    std::array<char, 64> threshold_text{};
    const auto formatted = std::to_chars(threshold_text.data(),
        threshold_text.data() + threshold_text.size(), threshold_percent);
    long double comparison_threshold{};
    const auto parsed = std::from_chars(threshold_text.data(), formatted.ptr, comparison_threshold);
    if (formatted.ec != std::errc{} || parsed.ec != std::errc{} || parsed.ptr != formatted.ptr) {
        return std::unexpected(Error{"collect_count", "threshold", "invalid_threshold",
            "Collection count threshold could not be converted to a decimal percentage"});
    }

    CollectCountAnalysis result;
    result.threshold_percent = threshold_percent;
    result.equipment_count = counts.size();
    result.status = counts.empty() ? CollectCountStatus::no_data : CollectCountStatus::ok;
    std::unordered_set<std::string_view> equipment;
    equipment.reserve(counts.size());
    for (const auto& pair : counts) {
        if (pair.eqp_ip.empty() || pair.eqp_ip.size() > 39
            || std::any_of(pair.eqp_ip.begin(), pair.eqp_ip.end(), [](unsigned char value) {
                return value < 32 || value == 127;
            })) {
            return std::unexpected(Error{"collect_count", "eqp_ip", "invalid_eqp_ip",
                "Equipment IP must contain 1 to 39 bytes without control characters"});
        }
        if (!equipment.insert(pair.eqp_ip).second) {
            return std::unexpected(Error{"collect_count", pair.eqp_ip, "duplicate_eqp_ip",
                "Collection counts contain duplicate equipment IPs"});
        }
        if ((!pair.current_present && pair.current_count != 0)
            || (!pair.previous_present && pair.previous_count != 0)
            || (!pair.current_present && !pair.previous_present)) {
            return std::unexpected(Error{"collect_count", pair.eqp_ip, "invalid_count_pair",
                "Absent collection counts must be zero and at least one day must be present"});
        }
        if (pair.current_count == pair.previous_count) {
            continue;
        }

        CollectCountAlert alert;
        alert.eqp_ip = pair.eqp_ip;
        alert.current_count = pair.current_count;
        alert.previous_count = pair.previous_count;
        alert.current_present = pair.current_present;
        alert.previous_present = pair.previous_present;
        if (pair.previous_count == 0) {
            alert.reason = "baseline_zero";
        } else {
            const bool increased = pair.current_count > pair.previous_count;
            const auto difference = increased ? pair.current_count - pair.previous_count
                : pair.previous_count - pair.current_count;
            const long double magnitude = static_cast<long double>(difference) * 100.0L
                / static_cast<long double>(pair.previous_count);
            if (magnitude < comparison_threshold) {
                continue;
            }
            alert.change_percent = static_cast<double>(increased ? magnitude : -magnitude);
            alert.reason = increased ? "increase" : "decrease";
        }
        result.alerts.push_back(std::move(alert));
    }
    if (!result.alerts.empty()) {
        result.status = CollectCountStatus::warning;
    }
    return result;
}

const char* to_string(CollectCountStatus status) noexcept {
    switch (status) {
        case CollectCountStatus::disabled: return "disabled";
        case CollectCountStatus::pending: return "pending";
        case CollectCountStatus::ok: return "ok";
        case CollectCountStatus::warning: return "warning";
        case CollectCountStatus::no_data: return "no_data";
        case CollectCountStatus::error: return "error";
    }
    return "error";
}

}
