// src/application/LogVolumeMonitor.cpp
#include "application/LogVolumeMonitor.hpp"

#include <tuple>
#include <utility>

namespace monitor {
namespace {

bool same_error(const std::optional<Error>& left, const std::optional<Error>& right) {
    if (left.has_value() != right.has_value()) return false;
    if (!left) return true;
    return std::tie(left->component, left->target, left->code, left->message) ==
        std::tie(right->component, right->target, right->code, right->message);
}

bool same_result(const LogVolumeAnalysis& left, const LogVolumeAnalysis& right) {
    return left.status == right.status && left.mode == right.mode && left.window == right.window &&
        left.queried_at == right.queried_at && same_error(left.error, right.error);
}

}

LogVolumeMonitor::LogVolumeMonitor(ILogVolumeReader& reader, const IMonitorSettingsProvider& settings,
    const ILogVolumeCalendar& calendar) : reader_(reader), settings_(settings), calendar_(calendar) {}

bool LogVolumeMonitor::tick(std::chrono::system_clock::time_point timestamp) {
    const auto& settings = settings_.current();
    LogVolumeAnalysis next;
    next.mode = settings.log_volume.mode;
    if (!settings.is_analyzer) {
        current_ = std::move(next);
        last_emitted_.reset();
        return false;
    }
    const auto desired = calendar_.window(timestamp, settings.log_volume);
    if (!desired) {
        next.status = LogVolumeStatus::error;
        next.error = desired.error();
    } else {
        next.status = LogVolumeStatus::pending;
        next.window = *desired;
        auto reading = reader_.read_log_volume(*desired);
        if (reading.window == next.window) {
            next.queried_at = reading.queried_at;
            if (reading.status == LogVolumeReadStatus::error || reading.error) {
                next.status = LogVolumeStatus::error;
                next.error = reading.error.value_or(Error{"log_volume", "query", "query_failed",
                    "Log volume totals could not be read"});
            } else if (reading.status == LogVolumeReadStatus::ready) {
                next.status = reading.equipment.empty() ? LogVolumeStatus::no_data : LogVolumeStatus::ok;
                next.equipment = std::move(reading.equipment);
            }
        }
    }
    current_ = std::move(next);
    if (current_.status == LogVolumeStatus::pending) return false;
    if (last_emitted_ && same_result(*last_emitted_, current_)) return false;
    last_emitted_ = current_;
    return true;
}

LogVolumeAnalysis LogVolumeMonitor::collect() {
    return current_;
}

}
