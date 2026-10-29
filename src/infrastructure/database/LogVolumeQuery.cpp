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

bool valid_bucket(std::uint64_t value) {
    return value >= 100001010000ULL && value <= 999912312359ULL &&
        parse_day(value / 10000).has_value() && value / 100 % 100 < 24 && value % 100 < 60 && value % 5 == 0;
}

bool matches_local_time(std::chrono::system_clock::time_point timestamp,
    std::uint64_t bucket) {
    if (timestamp.time_since_epoch() % std::chrono::seconds{1} !=
        std::chrono::system_clock::duration::zero()) return false;
    const auto value = std::chrono::system_clock::to_time_t(timestamp);
    std::tm local{};
    if (::localtime_r(&value, &local) == nullptr || local.tm_year < -900 || local.tm_year > 8099 ||
        local.tm_sec != 0) return false;
    const auto day = static_cast<std::uint64_t>(local.tm_year + 1900) * 10000ULL +
        static_cast<std::uint64_t>(local.tm_mon + 1) * 100ULL + static_cast<std::uint64_t>(local.tm_mday);
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
        "Log volume queries require aligned five-minute boundaries spanning at most one hour"};
    if (window.start_bucket >= window.end_bucket || window.start >= window.end ||
        !valid_bucket(window.start_bucket) || !valid_bucket(window.end_bucket)) {
        return std::unexpected(invalid);
    }
    ::tzset();
    if (!matches_local_time(window.start, window.start_bucket) ||
        !matches_local_time(window.end, window.end_bucket)) return std::unexpected(invalid);
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(window.end.time_since_epoch()) -
        std::chrono::duration_cast<std::chrono::seconds>(window.start.time_since_epoch());
    if (elapsed < std::chrono::minutes{5} || elapsed > std::chrono::hours{1} ||
        elapsed.count() % 300 != 0) return std::unexpected(invalid);
    return std::string{"SELECT eqp_ip, SUM(`count`), COUNT(*), COUNT(`count`), MIN(`count`), "}
        + "recv_time - MOD(recv_time, 5) AS interval_start FROM CloudESM.SUM_COLLECT_INFO"
        + " WHERE recv_time >= " + std::to_string(window.start_bucket) + " AND recv_time < " +
        std::to_string(window.end_bucket) + " GROUP BY eqp_ip, interval_start ORDER BY interval_start, eqp_ip LIMIT " +
        std::to_string(log_volume_max_rows + 1);
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
    if (!bucket || !valid_bucket(*bucket) || *bucket < window_.start_bucket || *bucket >= window_.end_bucket) {
        return std::unexpected(invalid_result());
    }
    std::string equipment{*row[0]};
    if (!keys_.insert({equipment, *bucket}).second) return std::unexpected(invalid_result());
    equipment_.insert(equipment);
    if (equipment_.size() > log_volume_max_equipment) {
        return std::unexpected(Error{"log_volume", "equipment", "equipment_limit_exceeded",
            "Log volume query exceeded the equipment limit"});
    }
    counts_.push_back({std::move(equipment), *sum, *bucket});
    return {};
}

std::vector<EquipmentLogVolume> LogVolumeQueryResult::take() && {
    return std::move(counts_);
}

}
