// src/infrastructure/linux/LocalLogVolumeCalendar.cpp
#include "infrastructure/linux/LocalLogVolumeCalendar.hpp"

#include <ctime>

namespace monitor {
namespace {

using Clock = std::chrono::system_clock;

Error calendar_error() {
    return {"log_volume", "calendar", "invalid_window", "Unable to determine the local log volume interval"};
}

std::optional<Clock::time_point> from_seconds(std::chrono::seconds::rep value) {
    const auto minimum = std::chrono::duration_cast<std::chrono::seconds>(Clock::duration::min()).count();
    const auto maximum = std::chrono::duration_cast<std::chrono::seconds>(Clock::duration::max()).count();
    if (value < minimum || value > maximum) return std::nullopt;
    return Clock::time_point{std::chrono::duration_cast<Clock::duration>(std::chrono::seconds{value})};
}

std::optional<std::tm> local_time(Clock::time_point timestamp) {
    const auto epoch = Clock::to_time_t(timestamp);
    std::tm local{};
    if (::localtime_r(&epoch, &local) == nullptr || local.tm_year < -900 || local.tm_year > 8099) {
        return std::nullopt;
    }
    return local;
}

std::uint64_t date_bucket(std::chrono::year_month_day date) {
    return static_cast<std::uint64_t>(int(date.year())) * 10000ULL +
        static_cast<std::uint64_t>(unsigned(date.month())) * 100ULL + unsigned(date.day());
}

std::chrono::year_month_day local_date(const std::tm& local) {
    return std::chrono::year{local.tm_year + 1900}/
        std::chrono::month{static_cast<unsigned>(local.tm_mon + 1)}/
        std::chrono::day{static_cast<unsigned>(local.tm_mday)};
}

std::uint64_t minute_bucket(const std::tm& local) {
    return date_bucket(local_date(local)) * 10000ULL +
        static_cast<unsigned>(local.tm_hour) * 100ULL + static_cast<unsigned>(local.tm_min);
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

std::optional<Clock::time_point> at_local_time(std::chrono::year_month_day date, unsigned minutes) {
    if (!date.ok() || int(date.year()) < 1000 || int(date.year()) > 9999) return std::nullopt;
    std::tm local{};
    local.tm_year = int(date.year()) - 1900;
    local.tm_mon = static_cast<int>(unsigned(date.month())) - 1;
    local.tm_mday = static_cast<int>(unsigned(date.day()));
    local.tm_hour = static_cast<int>(minutes / 60);
    local.tm_min = static_cast<int>(minutes % 60);
    local.tm_isdst = -1;
    const auto epoch = ::mktime(&local);
    return from_seconds(static_cast<std::chrono::seconds::rep>(epoch));
}

}

std::expected<LogVolumeWindow, Error> LocalLogVolumeCalendar::window(
    Clock::time_point timestamp, const LogVolumeSettings& settings) const {
    if (settings.interval_seconds < 60 || settings.interval_seconds > 86400 ||
        settings.interval_seconds % 60 != 0 || settings.daily_at_minutes >= 1440) {
        return std::unexpected(calendar_error());
    }
    ::tzset();
    if (settings.mode == LogVolumeMode::interval) {
        const auto seconds = std::chrono::floor<std::chrono::seconds>(timestamp).time_since_epoch().count();
        const auto period = static_cast<std::chrono::seconds::rep>(settings.interval_seconds);
        const auto remainder = (seconds % period + period) % period;
        const auto end = from_seconds(seconds - remainder);
        const auto start = from_seconds(seconds - remainder - period);
        if (!start || !end) return std::unexpected(calendar_error());
        const auto local_start = local_time(*start);
        const auto local_end = local_time(*end);
        if (!local_start || !local_end) return std::unexpected(calendar_error());
        const auto start_bucket = minute_bucket(*local_start);
        const auto end_bucket = minute_bucket(*local_end);
        if (start_bucket >= end_bucket || ambiguous_local_time(*local_start) || ambiguous_local_time(*local_end)) {
            return std::unexpected(Error{"log_volume", "calendar", "ambiguous_interval",
                "The local log volume interval cannot be represented uniquely by minute timestamps"});
        }
        return LogVolumeWindow{settings.mode, start_bucket, end_bucket, *start, *end};
    }
    if (settings.mode != LogVolumeMode::daily) return std::unexpected(calendar_error());
    const auto local = local_time(timestamp);
    if (!local) return std::unexpected(calendar_error());
    const auto today = local_date(*local);
    if (!today.ok()) return std::unexpected(calendar_error());
    const auto scheduled = at_local_time(today, settings.daily_at_minutes);
    if (!scheduled) return std::unexpected(calendar_error());
    const auto end_day = std::chrono::sys_days{today} - std::chrono::days{timestamp < *scheduled ? 1 : 0};
    const std::chrono::year_month_day end_date{end_day};
    const std::chrono::year_month_day start_date{end_day - std::chrono::days{1}};
    const auto start = at_local_time(start_date, 0);
    const auto end = at_local_time(end_date, 0);
    if (!start || !end || *start >= *end || *end > timestamp) return std::unexpected(calendar_error());
    return LogVolumeWindow{settings.mode, date_bucket(start_date), date_bucket(end_date), *start, *end};
}

}
