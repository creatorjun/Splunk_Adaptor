// src/infrastructure/database/LogVolumeQuery.cpp
#include "infrastructure/database/LogVolumeQuery.hpp"

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

std::optional<std::chrono::sys_days> parse_day(std::uint64_t value) {
    if (value < 10000101 || value > 99991231) return std::nullopt;
    const std::chrono::year_month_day day{
        std::chrono::year{static_cast<int>(value / 10000)},
        std::chrono::month{static_cast<unsigned>(value / 100 % 100)},
        std::chrono::day{static_cast<unsigned>(value % 100)}};
    if (!day.ok()) return std::nullopt;
    return std::chrono::sys_days{day};
}

bool valid_bucket(std::uint64_t value, LogVolumeMode mode) {
    if (mode == LogVolumeMode::daily) return parse_day(value).has_value();
    return value >= 100001010000ULL && value <= 999912312359ULL &&
        parse_day(value / 10000).has_value() && value / 100 % 100 < 24 && value % 100 < 60;
}

bool matches_local_time(std::chrono::system_clock::time_point timestamp,
    std::uint64_t bucket, LogVolumeMode mode) {
    if (timestamp.time_since_epoch() % std::chrono::seconds{1} !=
        std::chrono::system_clock::duration::zero()) return false;
    const auto value = std::chrono::system_clock::to_time_t(timestamp);
    std::tm local{};
    if (::localtime_r(&value, &local) == nullptr || local.tm_year < -900 || local.tm_year > 8099 ||
        local.tm_sec != 0) return false;
    const auto day = static_cast<std::uint64_t>(local.tm_year + 1900) * 10000ULL +
        static_cast<std::uint64_t>(local.tm_mon + 1) * 100ULL + static_cast<std::uint64_t>(local.tm_mday);
    if (mode == LogVolumeMode::daily) {
        return bucket == day && local.tm_hour == 0 && local.tm_min == 0;
    }
    return bucket == day * 10000ULL + static_cast<std::uint64_t>(local.tm_hour) * 100ULL +
        static_cast<std::uint64_t>(local.tm_min);
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
    const auto invalid = Error{"log_volume", "window", "invalid_window",
        "Log volume queries require a coherent local minute interval or one complete calendar day"};
    if ((window.mode != LogVolumeMode::interval && window.mode != LogVolumeMode::daily) ||
        window.start_bucket >= window.end_bucket || window.start >= window.end ||
        !valid_bucket(window.start_bucket, window.mode) || !valid_bucket(window.end_bucket, window.mode)) {
        return std::unexpected(invalid);
    }
    ::tzset();
    if (!matches_local_time(window.start, window.start_bucket, window.mode) ||
        !matches_local_time(window.end, window.end_bucket, window.mode)) return std::unexpected(invalid);
    if (window.mode == LogVolumeMode::daily) {
        if (*parse_day(window.end_bucket) - *parse_day(window.start_bucket) != std::chrono::days{1}) {
            return std::unexpected(invalid);
        }
    } else {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(window.end.time_since_epoch()) -
            std::chrono::duration_cast<std::chrono::seconds>(window.start.time_since_epoch());
        if (elapsed < std::chrono::seconds{60} || elapsed > std::chrono::seconds{86400} ||
            elapsed.count() % 60 != 0) return std::unexpected(invalid);
    }
    const auto* source = window.mode == LogVolumeMode::daily
        ? "CloudESM.SUM_COLLECT_INFO_1DAY" : "CloudESM.SUM_COLLECT_INFO";
    return std::string{"SELECT eqp_ip, SUM(`count`), COUNT(*), COUNT(`count`), MIN(`count`) FROM "} +
        source + " WHERE recv_time >= " + std::to_string(window.start_bucket) + " AND recv_time < " +
        std::to_string(window.end_bucket) + " GROUP BY eqp_ip ORDER BY eqp_ip LIMIT " +
        std::to_string(log_volume_max_equipment + 1);
}

std::expected<void, Error> LogVolumeQueryResult::append(const LogVolumeQueryRow& row) {
    if (counts_.size() >= log_volume_max_equipment) {
        return std::unexpected(Error{"log_volume", "equipment", "equipment_limit_exceeded",
            "Log volume query exceeded the equipment limit"});
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
    std::string equipment{*row[0]};
    if (!equipment_.insert(equipment).second) return std::unexpected(invalid_result());
    counts_.push_back({std::move(equipment), *sum});
    return {};
}

std::vector<EquipmentLogVolume> LogVolumeQueryResult::take() && {
    return std::move(counts_);
}

}
