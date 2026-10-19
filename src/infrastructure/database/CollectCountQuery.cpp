// src/infrastructure/database/CollectCountQuery.cpp
#include "infrastructure/database/CollectCountQuery.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <utility>

namespace monitor {
namespace {

Error invalid_result() {
    return {"collect_count", "CloudESM.SUM_COLLECT_INFO_1DAY", "invalid_count_data",
        "Daily collection counts contain invalid, missing, negative, or overflowing values"};
}

std::optional<std::chrono::sys_days> parse_day(std::uint32_t value) {
    if (value < 10000101 || value > 99991231) {
        return std::nullopt;
    }
    const std::chrono::year_month_day day{
        std::chrono::year{static_cast<int>(value / 10000)},
        std::chrono::month{value / 100 % 100}, std::chrono::day{value % 100}};
    if (!day.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{day};
}

std::expected<std::uint64_t, Error> unsigned_number(const std::optional<std::string_view>& field) {
    if (!field || field->empty() || field->size() > 20
        || !std::all_of(field->begin(), field->end(), [](char value) { return value >= '0' && value <= '9'; })) {
        return std::unexpected(invalid_result());
    }
    std::uint64_t value{};
    const auto parsed = std::from_chars(field->data(), field->data() + field->size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != field->data() + field->size()) {
        return std::unexpected(invalid_result());
    }
    return value;
}

std::expected<std::pair<std::uint64_t, bool>, Error> decode_day(
    const CollectCountQueryRow& row, std::size_t sum_index, std::size_t rows_index,
    std::size_t nonnull_index, std::size_t minimum_index) {
    const auto sum = unsigned_number(row[sum_index]);
    const auto rows = unsigned_number(row[rows_index]);
    const auto nonnull = unsigned_number(row[nonnull_index]);
    if (!sum || !rows || !nonnull || *rows != *nonnull) {
        return std::unexpected(invalid_result());
    }
    if (*rows == 0) {
        if (*sum != 0 || row[minimum_index]) {
            return std::unexpected(invalid_result());
        }
        return std::pair<std::uint64_t, bool>{0, false};
    }
    const auto minimum = unsigned_number(row[minimum_index]);
    if (!minimum || *minimum > *sum) {
        return std::unexpected(invalid_result());
    }
    return std::pair<std::uint64_t, bool>{*sum, true};
}

}

std::expected<std::string, Error> make_collect_count_query(const DailyCountWindow& window) {
    const auto current = parse_day(window.current_day);
    const auto previous = parse_day(window.previous_day);
    if (!current || !previous || *current - *previous != std::chrono::days{1}) {
        return std::unexpected(Error{"collect_count", "CloudESM.SUM_COLLECT_INFO_1DAY", "invalid_day_window",
            "Daily collection count queries require two adjacent valid calendar dates"});
    }
    const auto current_day = std::to_string(window.current_day);
    const auto previous_day = std::to_string(window.previous_day);
    const auto current_case = "CASE WHEN recv_time = " + current_day + " THEN ";
    const auto previous_case = "CASE WHEN recv_time = " + previous_day + " THEN ";
    return "SELECT eqp_ip, SUM(" + current_case + "`count` ELSE 0 END), SUM("
        + previous_case + "`count` ELSE 0 END), COUNT(" + current_case + "1 END), COUNT("
        + previous_case + "1 END), COUNT(" + current_case + "`count` END), COUNT("
        + previous_case + "`count` END), MIN(" + current_case + "`count` END), MIN("
        + previous_case + "`count` END) FROM CloudESM.SUM_COLLECT_INFO_1DAY WHERE recv_time IN ("
        + previous_day + ", " + current_day + ") GROUP BY eqp_ip ORDER BY eqp_ip LIMIT "
        + std::to_string(collect_count_max_equipment + 1);
}

std::expected<void, Error> CollectCountQueryResult::append(const CollectCountQueryRow& row) {
    if (counts_.size() >= collect_count_max_equipment) {
        return std::unexpected(Error{"collect_count", "CloudESM.SUM_COLLECT_INFO_1DAY", "equipment_limit_exceeded",
            "Daily collection count query exceeded the equipment limit"});
    }
    if (!row[0] || row[0]->empty() || row[0]->size() > 39
        || std::any_of(row[0]->begin(), row[0]->end(), [](unsigned char value) { return value < 32 || value == 127; })) {
        return std::unexpected(invalid_result());
    }
    const auto current = decode_day(row, 1, 3, 5, 7);
    const auto previous = decode_day(row, 2, 4, 6, 8);
    if (!current || !previous || (!current->second && !previous->second)) {
        return std::unexpected(invalid_result());
    }
    std::string equipment{*row[0]};
    if (!equipment_.insert(equipment).second) {
        return std::unexpected(invalid_result());
    }
    counts_.push_back({std::move(equipment), current->first, previous->first, current->second, previous->second});
    return {};
}

std::vector<DailyCountPair> CollectCountQueryResult::take() && {
    return std::move(counts_);
}

}
