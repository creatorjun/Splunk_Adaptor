// src/application/CollectCountMonitor.cpp
#include "application/CollectCountMonitor.hpp"

#include <utility>

namespace monitor {

CollectCountMonitor::CollectCountMonitor(ICollectCountReader& reader,
    const IMonitorSettingsProvider& settings, const ILocalDayWindow& calendar)
    : reader_(reader), settings_(settings), calendar_(calendar) {}

void CollectCountMonitor::settings_changed(std::chrono::system_clock::time_point timestamp) {
    const auto& settings = settings_.current();
    if (settings.role != ServerRole::analyzer) return;
    const auto window = calendar_.make(timestamp, settings.analyzer.comparison_day);
    if (window) reader_.read_daily_counts(*window, settings.collection_interval);
}

CollectCountAnalysis CollectCountMonitor::collect(std::chrono::system_clock::time_point timestamp) {
    const auto& settings = settings_.current();
    CollectCountAnalysis result;
    result.threshold_percent = settings.analyzer.collect_count_change_percent;
    if (settings.role != ServerRole::analyzer) return result;
    const auto window = calendar_.make(timestamp, settings.analyzer.comparison_day);
    if (!window) {
        result.status = CollectCountStatus::error;
        result.error = window.error();
        return result;
    }
    result.window = *window;
    auto reading = reader_.read_daily_counts(*window, settings.collection_interval);
    result.queried_at = reading.queried_at;
    if (reading.status == CollectCountReadStatus::error || reading.error) {
        result.status = CollectCountStatus::error;
        result.error = reading.error.value_or(Error{"collect_count", "CloudESM.SUM_COLLECT_INFO_1DAY",
            "query_failed", "Daily collection counts could not be read"});
        return result;
    }
    if (reading.status != CollectCountReadStatus::ready) {
        result.status = CollectCountStatus::pending;
        return result;
    }
    auto evaluated = analyze_collect_counts(reading.counts, result.threshold_percent);
    if (!evaluated) {
        result.status = CollectCountStatus::error;
        result.error = evaluated.error();
        return result;
    }
    evaluated->window = result.window;
    evaluated->queried_at = result.queried_at;
    return std::move(*evaluated);
}

}
