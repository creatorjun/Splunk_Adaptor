// src/infrastructure/linux/LocalLogVolumeCalendar.cpp
#include "infrastructure/linux/LocalLogVolumeCalendar.hpp"
#include "domain/LogCountPolicy.hpp"

#include <ctime>

namespace monitor {
namespace {

using Clock = std::chrono::system_clock;

Error calendar_error() {
    return {"log_volume", "calendar", "invalid_window", "Unable to determine a local five-minute log volume range"};
}

std::optional<std::tm> local_time(Clock::time_point timestamp) {
    const auto epoch = Clock::to_time_t(timestamp);
    std::tm local{};
    if (::localtime_r(&epoch, &local) == nullptr || local.tm_year < -900 || local.tm_year > 8099) {
        return std::nullopt;
    }
    return local;
}

bool same_local_minute(const std::tm& left, const std::tm& right) {
    return left.tm_year == right.tm_year && left.tm_mon == right.tm_mon && left.tm_mday == right.tm_mday &&
        left.tm_hour == right.tm_hour && left.tm_min == right.tm_min && left.tm_sec == right.tm_sec;
}

bool ambiguous_local_time(const std::tm& local) {
    std::optional<std::time_t> first;
    for (const int daylight : {0, 1}) {
        auto candidate = local;
        candidate.tm_isdst = daylight;
        const auto epoch = ::mktime(&candidate);
        std::tm actual{};
        if (::localtime_r(&epoch, &actual) == nullptr || !same_local_minute(actual, local)) continue;
        if (first && *first != epoch) return true;
        first = epoch;
    }
    return false;
}

}

std::expected<LogVolumeWindow, Error> LocalLogVolumeCalendar::window(
    Clock::time_point timestamp) const {
    const auto elapsed = std::chrono::floor<std::chrono::seconds>(timestamp).time_since_epoch();
    const auto period = LogCountPolicy::query_interval.count();
    const auto remainder = (elapsed.count() % period + period) % period;
    const auto end_seconds = elapsed.count() - remainder;
    const auto minimum = std::chrono::duration_cast<std::chrono::seconds>(Clock::duration::min()).count();
    const auto maximum = std::chrono::duration_cast<std::chrono::seconds>(Clock::duration::max()).count();
    if (end_seconds < minimum + period || end_seconds > maximum) return std::unexpected(calendar_error());
    const auto end = Clock::time_point{std::chrono::seconds{end_seconds}};
    return range(end - LogCountPolicy::query_interval, end);
}

std::expected<LogVolumeWindow, Error> LocalLogVolumeCalendar::range(Clock::time_point start,
    Clock::time_point end) const {
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(end.time_since_epoch()) -
        std::chrono::duration_cast<std::chrono::seconds>(start.time_since_epoch());
    if (start >= end || elapsed > LogCountPolicy::maximum_query_range ||
        elapsed % LogCountPolicy::query_interval != std::chrono::seconds::zero() ||
        start.time_since_epoch() % std::chrono::seconds{1} != Clock::duration::zero() ||
        end.time_since_epoch() % std::chrono::seconds{1} != Clock::duration::zero()) {
        return std::unexpected(calendar_error());
    }
    ::tzset();
    const auto local_start = local_time(start);
    const auto local_end = local_time(end);
    if (!local_start || !local_end || local_start->tm_sec != 0 || local_end->tm_sec != 0 ||
        local_start->tm_min % std::chrono::duration_cast<std::chrono::minutes>(LogCountPolicy::query_interval).count() != 0 ||
        local_end->tm_min % std::chrono::duration_cast<std::chrono::minutes>(LogCountPolicy::query_interval).count() != 0) return std::unexpected(calendar_error());
    for (auto cursor = start;; cursor += LogCountPolicy::query_interval) {
        const auto local = local_time(cursor);
        if (!local || ambiguous_local_time(*local)) {
            return std::unexpected(Error{"log_volume", "calendar", "ambiguous_interval",
                "Local minute timestamps cannot uniquely identify this log volume range"});
        }
        if (cursor == end) break;
    }
    return LogVolumeWindow{start, end};
}

std::expected<LogCountHour, Error> LocalLogVolumeCalendar::hour(Clock::time_point timestamp) const {
    ::tzset();
    auto local = local_time(timestamp);
    if (!local || ambiguous_local_time(*local)) return std::unexpected(calendar_error());
    local->tm_min = 0;
    local->tm_sec = 0;
    const auto start = Clock::from_time_t(::mktime(&*local));
    if (start > Clock::time_point::max() - LogCountPolicy::report_interval) return std::unexpected(calendar_error());
    const auto end = start + LogCountPolicy::report_interval;
    const auto end_local = local_time(end);
    if (timestamp < start || timestamp >= end || !end_local || end_local->tm_min != 0 || end_local->tm_sec != 0) {
        return std::unexpected(calendar_error());
    }
    return LogCountHour{start, end, timestamp, 0, {}};
}

}
