// src/infrastructure/database/LogSilenceQuery.hpp
#pragma once

#include "domain/LogSilenceAnalysis.hpp"

#include <array>
#include <map>
#include <set>
#include <span>
#include <string_view>

namespace monitor {

inline constexpr std::size_t log_silence_max_equipment = 10000;
using LogSilenceQueryRow = std::array<std::optional<std::string_view>, 4>;

std::string make_log_silence_query(bool indexed = false);

struct LogSilenceIndexColumns {
    std::array<std::size_t, 5> required;
    std::optional<std::size_t> ignored;
    std::optional<std::size_t> visible;
};

using LogSilenceIndexRow = std::array<std::optional<std::string_view>, 7>;

std::expected<LogSilenceIndexColumns, Error> find_log_silence_index_columns(
    std::span<const std::string_view> fields);

class LogSilenceIndexCatalog {
public:
    std::expected<void, Error> append(const LogSilenceIndexRow& row);
    bool supports_latest_lookup() const;

private:
    struct Index {
        std::map<unsigned, bool> columns;
        bool usable{true};
    };
    std::map<std::string, Index> indexes_;
    std::size_t rows_{};
};

class LogSilenceQueryResult {
public:
    std::expected<void, Error> append(const LogSilenceQueryRow& row);
    std::expected<LogReceiptBatch, Error> take() &&;

private:
    std::set<std::string> received_;
    std::optional<std::chrono::system_clock::time_point> observed_at_;
    LogReceiptBatch batch_;
    bool empty_result_{};
};

}
