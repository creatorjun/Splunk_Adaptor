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

Error invalid_index_metadata() {
    return {"log_silence", "CloudESM.SUM_COLLECT_INFO", "invalid_index_metadata",
        "Last log receipt index metadata are invalid or unsupported"};
}

bool equal_ascii_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lower = [](char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        };
        if (lower(left[index]) != lower(right[index])) return false;
    }
    return true;
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

std::string make_log_silence_query(bool indexed) {
    const auto limit = std::to_string(log_silence_max_equipment + 1);
    const std::string receipts = indexed
        ? "SELECT equipment.eqp_ip, (SELECT recent.recv_time FROM CloudESM.SUM_COLLECT_INFO AS recent "
          "WHERE recent.eqp_ip = equipment.eqp_ip AND recent.`count` > 0 "
          "ORDER BY recent.recv_time DESC LIMIT 1) AS last_bucket FROM "
          "(SELECT DISTINCT eqp_ip FROM CloudESM.SUM_COLLECT_INFO "
          "WHERE eqp_ip IS NOT NULL AND eqp_ip <> '' ORDER BY eqp_ip LIMIT " + limit + ") AS equipment "
          "ORDER BY equipment.eqp_ip LIMIT " + limit
        : "SELECT eqp_ip, MAX(CASE WHEN `count` > 0 THEN recv_time END) AS last_bucket "
          "FROM CloudESM.SUM_COLLECT_INFO WHERE eqp_ip IS NOT NULL AND eqp_ip <> '' GROUP BY eqp_ip";
    return "SELECT received.eqp_ip, received.last_bucket, UNIX_TIMESTAMP(STR_TO_DATE(CAST("
        "received.last_bucket AS CHAR), '%Y%m%d%H%i')), metadata.observed_at FROM "
        "(SELECT UNIX_TIMESTAMP() AS observed_at) AS metadata LEFT JOIN "
        "(" + receipts + ") AS received ON 1 = 1 ORDER BY received.eqp_ip LIMIT " + limit;
}

std::expected<LogSilenceIndexColumns, Error> find_log_silence_index_columns(
    std::span<const std::string_view> fields) {
    constexpr std::array<std::string_view, 5> names{
        "Key_name", "Seq_in_index", "Column_name", "Sub_part", "Index_type"};
    LogSilenceIndexColumns columns;
    columns.required.fill(fields.size());
    for (std::size_t index = 0; index < fields.size(); ++index) {
        for (std::size_t selected = 0; selected < names.size(); ++selected) {
            if (equal_ascii_case(fields[index], names[selected])) {
                if (columns.required[selected] != fields.size()) return std::unexpected(invalid_index_metadata());
                columns.required[selected] = index;
            }
        }
        if (equal_ascii_case(fields[index], "Ignored")) {
            if (columns.ignored) return std::unexpected(invalid_index_metadata());
            columns.ignored = index;
        }
        if (equal_ascii_case(fields[index], "Visible")) {
            if (columns.visible) return std::unexpected(invalid_index_metadata());
            columns.visible = index;
        }
    }
    if (std::find(columns.required.begin(), columns.required.end(), fields.size()) != columns.required.end()) {
        return std::unexpected(invalid_index_metadata());
    }
    return columns;
}

std::expected<void, Error> LogSilenceIndexCatalog::append(const LogSilenceIndexRow& row) {
    const auto sequence = unsigned_number(row[1]);
    if (++rows_ > 4096 || !row[0] || row[0]->empty() || row[0]->size() > 256 ||
        !sequence || *sequence == 0 || *sequence > 1024 || !row[4] || row[4]->empty()) {
        return std::unexpected(invalid_index_metadata());
    }
    auto& index = indexes_[std::string{*row[0]}];
    const auto part = static_cast<unsigned>(*sequence);
    constexpr std::array<std::string_view, 3> prefix{"eqp_ip", "recv_time", "count"};
    const bool matches = part > prefix.size() ||
        (row[2] && equal_ascii_case(*row[2], prefix[part - 1]) && !row[3]);
    if (!index.columns.emplace(part, matches).second) return std::unexpected(invalid_index_metadata());
    index.usable = index.usable && equal_ascii_case(*row[4], "BTREE");
    if (row[5]) {
        if (!equal_ascii_case(*row[5], "YES") && !equal_ascii_case(*row[5], "NO")) {
            return std::unexpected(invalid_index_metadata());
        }
        index.usable = index.usable && equal_ascii_case(*row[5], "NO");
    }
    if (row[6]) {
        if (!equal_ascii_case(*row[6], "YES") && !equal_ascii_case(*row[6], "NO")) {
            return std::unexpected(invalid_index_metadata());
        }
        index.usable = index.usable && equal_ascii_case(*row[6], "YES");
    }
    return {};
}

bool LogSilenceIndexCatalog::supports_latest_lookup() const {
    for (const auto& [name, index] : indexes_) {
        static_cast<void>(name);
        if (!index.usable || index.columns.size() < 3) continue;
        unsigned expected{1};
        bool complete{true};
        for (const auto& [sequence, matches] : index.columns) {
            if (sequence != expected++ || !matches) {
                complete = false;
                break;
            }
        }
        if (complete) return true;
    }
    return false;
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
