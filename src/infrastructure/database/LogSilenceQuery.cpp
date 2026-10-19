// src/infrastructure/database/LogSilenceQuery.cpp
#include "infrastructure/database/LogSilenceQuery.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <utility>

namespace monitor {
namespace {

Error invalid_result() {
    return {"log_silence", "CloudESM.SUM_COLLECT_INFO", "invalid_receipt_data",
        "Last log receipt data contain invalid, missing, inconsistent, or overflowing values"};
}

bool valid_equipment(const std::string_view value) {
    return !value.empty() && value.size() <= 39
        && std::none_of(value.begin(), value.end(), [](unsigned char character) {
            return character < 32 || character == 127;
        });
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

std::expected<std::chrono::system_clock::time_point, Error> epoch(
    const std::optional<std::string_view>& field) {
    const auto value = unsigned_number(field);
    const auto maximum = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::duration::max()).count();
    if (!value || *value == 0 || *value > static_cast<std::uint64_t>(maximum)) {
        return std::unexpected(invalid_result());
    }
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::seconds{static_cast<std::chrono::seconds::rep>(*value)})};
}

bool valid_bucket(const std::optional<std::string_view>& field) {
    const auto value = unsigned_number(field);
    if (!value || field->size() != 12 || *value < 100001010000ULL || *value > 999912312359ULL) {
        return false;
    }
    const auto date = *value / 10000;
    const std::chrono::year_month_day day{
        std::chrono::year{static_cast<int>(date / 10000)},
        std::chrono::month{static_cast<unsigned>(date / 100 % 100)},
        std::chrono::day{static_cast<unsigned>(date % 100)}};
    return day.ok() && *value / 100 % 100 < 24 && *value % 100 < 60;
}

}

std::string make_log_silence_query() {
    return "SELECT received.eqp_ip, received.last_bucket, UNIX_TIMESTAMP(STR_TO_DATE(CAST("
        "received.last_bucket AS CHAR), '%Y%m%d%H%i')), metadata.observed_at FROM "
        "(SELECT UNIX_TIMESTAMP() AS observed_at) AS metadata LEFT JOIN "
        "(SELECT eqp_ip, MAX(CASE WHEN `count` > 0 THEN recv_time END) AS last_bucket "
        "FROM CloudESM.SUM_COLLECT_INFO WHERE eqp_ip IS NOT NULL AND eqp_ip <> '' "
        "GROUP BY eqp_ip) AS received ON 1 = 1 ORDER BY received.eqp_ip LIMIT "
        + std::to_string(log_silence_max_equipment + 1);
}
std::expected<void, Error> LogSilenceQueryResult::append(const LogSilenceQueryRow& row) {
    if (empty_result_ || batch_.records.size() >= log_silence_max_equipment) {
        return std::unexpected(invalid_result());
    }
    const auto observed = epoch(row[3]);
    if (!observed || (observed_at_ && *observed_at_ != *observed)) {
        return std::unexpected(invalid_result());
    }
    if (!row[0]) {
        if (row[1] || row[2] || !batch_.records.empty()) return std::unexpected(invalid_result());
        empty_result_ = true;
        observed_at_ = *observed;
        batch_.observed_at = *observed;
        return {};
    }
    if (!valid_equipment(*row[0])) return std::unexpected(invalid_result());
    LastLogReceipt receipt;
    receipt.eqp_ip = std::string{*row[0]};
    if (!row[1]) {
        if (row[2]) return std::unexpected(invalid_result());
    } else {
        const auto received = epoch(row[2]);
        if (!valid_bucket(row[1]) || !received) {
            return std::unexpected(invalid_result());
        }
        receipt.last_received_at = *received;
    }
    if (!received_.insert(receipt.eqp_ip).second) {
        return std::unexpected(invalid_result());
    }
    observed_at_ = *observed;
    batch_.observed_at = *observed;
    batch_.records.push_back(std::move(receipt));
    return {};
}

std::expected<LogReceiptBatch, Error> LogSilenceQueryResult::take() && {
    if (!observed_at_) {
        return std::unexpected(invalid_result());
    }
    return std::move(batch_);
}

}
