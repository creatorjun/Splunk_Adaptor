// src/infrastructure/database/LogVolumeQuery.cpp
#include "infrastructure/database/LogVolumeQuery.hpp"
#include "domain/LogCountPolicy.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <utility>

namespace monitor {
namespace {

Error invalid_result() {
    return {"log_volume", "equipment", "invalid_count_data",
        "Log volume results contain invalid, missing, negative, inconsistent, or overflowing values"};
}

using Clock = std::chrono::system_clock;

Error invalid_window() {
    return {"log_volume", "window", "invalid_window",
        "Log volume queries require aligned five-minute boundaries spanning at most one hour"};
}

bool same_local_minute(const std::tm& left, const std::tm& right) {
    return left.tm_year == right.tm_year && left.tm_mon == right.tm_mon && left.tm_mday == right.tm_mday &&
        left.tm_hour == right.tm_hour && left.tm_min == right.tm_min && left.tm_sec == right.tm_sec;
}

std::optional<std::uint64_t> encode_minute(Clock::time_point timestamp) {
    if (timestamp.time_since_epoch() % std::chrono::seconds{1} != Clock::duration::zero()) return std::nullopt;
    const auto epoch = Clock::to_time_t(timestamp);
    std::tm local{};
    if (::localtime_r(&epoch, &local) == nullptr || local.tm_year < -900 || local.tm_year > 8099 ||
        local.tm_sec != 0 || local.tm_min % std::chrono::duration_cast<std::chrono::minutes>(LogCountPolicy::query_interval).count() != 0) return std::nullopt;
    for (const int daylight : {0, 1}) {
        auto candidate = local;
        candidate.tm_isdst = daylight;
        const auto alternate = ::mktime(&candidate);
        std::tm actual{};
        if (alternate != epoch && ::localtime_r(&alternate, &actual) != nullptr && same_local_minute(actual, local)) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint64_t>(local.tm_year + 1900) * 100000000ULL +
        static_cast<std::uint64_t>(local.tm_mon + 1) * 1000000ULL +
        static_cast<std::uint64_t>(local.tm_mday) * 10000ULL +
        static_cast<std::uint64_t>(local.tm_hour) * 100ULL + static_cast<std::uint64_t>(local.tm_min);
}

std::expected<std::map<std::uint64_t, Clock::time_point>, Error> encode_window(const LogVolumeWindow& window) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(window.end.time_since_epoch()) -
        std::chrono::duration_cast<std::chrono::seconds>(window.start.time_since_epoch());
    if (window.start >= window.end || elapsed < LogCountPolicy::query_interval || elapsed > LogCountPolicy::maximum_query_range ||
        elapsed % LogCountPolicy::query_interval != std::chrono::seconds::zero()) return std::unexpected(invalid_window());
    ::tzset();
    std::map<std::uint64_t, Clock::time_point> intervals;
    for (auto timestamp = window.start;; timestamp += LogCountPolicy::query_interval) {
        const auto bucket = encode_minute(timestamp);
        if (!bucket || (!intervals.empty() && *bucket <= intervals.rbegin()->first)) {
            return std::unexpected(invalid_window());
        }
        intervals.emplace(*bucket, timestamp);
        if (timestamp == window.end) break;
        if (timestamp > window.end - LogCountPolicy::query_interval) return std::unexpected(invalid_window());
    }
    return intervals;
}

std::expected<std::uint64_t, Error> unsigned_number(const std::optional<std::string_view>& field) {
    if (!field || field->empty() || field->size() > 20 ||
        !std::all_of(field->begin(), field->end(), [](char value) { return value >= '0' && value <= '9'; })) {
        return std::unexpected(invalid_result());
    }
    std::uint64_t value{};
    const auto parsed = std::from_chars(field->data(), field->data() + field->size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != field->data() + field->size()) {
        return std::unexpected(invalid_result());
    }
    return value;
}

}

std::expected<std::string, Error> make_log_volume_query(const LogVolumeWindow& window) {
    const auto intervals = encode_window(window);
    if (!intervals) return std::unexpected(intervals.error());
    return std::string{"SELECT eqp_ip, SUM(`count`), COUNT(*), COUNT(`count`), MIN(`count`), "}
        + "recv_time - MOD(recv_time, " + std::to_string(std::chrono::duration_cast<std::chrono::minutes>(
            LogCountPolicy::query_interval).count()) + ") AS interval_start FROM CloudESM.SUM_COLLECT_INFO"
        + " WHERE recv_time >= " + std::to_string(intervals->begin()->first) + " AND recv_time < " +
        std::to_string(intervals->rbegin()->first) + " GROUP BY eqp_ip, interval_start ORDER BY interval_start, eqp_ip LIMIT " +
        std::to_string(log_volume_max_rows + 1);
}

LogVolumeQueryResult::LogVolumeQueryResult(LogVolumeWindow window) : window_(window) {
    auto intervals = encode_window(window);
    if (intervals) intervals_ = std::move(*intervals);
}

std::expected<void, Error> LogVolumeQueryResult::append(const LogVolumeQueryRow& row) {
    if (counts_.size() >= log_volume_max_rows) {
        return std::unexpected(Error{"log_volume", "equipment", "row_limit_exceeded",
            "Log volume query exceeded the grouped row limit"});
    }
    if (!row[0] || row[0]->empty() || row[0]->size() > 39 ||
        std::any_of(row[0]->begin(), row[0]->end(), [](unsigned char value) { return value < 32 || value == 127; })) {
        return std::unexpected(invalid_result());
    }
    const auto sum = unsigned_number(row[1]);
    const auto rows = unsigned_number(row[2]);
    const auto nonnull = unsigned_number(row[3]);
    const auto minimum = unsigned_number(row[4]);
    if (!sum || !rows || !nonnull || !minimum || *rows == 0 || *rows != *nonnull ||
        *minimum > *sum / *rows || (*rows == 1 && *minimum != *sum)) {
        return std::unexpected(invalid_result());
    }
    const auto bucket = unsigned_number(row[5]);
    const auto interval = bucket ? intervals_.find(*bucket) : intervals_.end();
    if (interval == intervals_.end() || interval->second >= window_.end) {
        return std::unexpected(invalid_result());
    }
    std::string equipment{*row[0]};
    if (!keys_.insert({equipment, *bucket}).second) return std::unexpected(invalid_result());
    equipment_.insert(equipment);
    if (equipment_.size() > log_volume_max_equipment) {
        return std::unexpected(Error{"log_volume", "equipment", "equipment_limit_exceeded",
            "Log volume query exceeded the equipment limit"});
    }
    counts_.push_back({std::move(equipment), *sum, interval->second});
    return {};
}

std::vector<EquipmentLogVolume> LogVolumeQueryResult::take() && {
    return std::move(counts_);
}

}
