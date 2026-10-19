// src/domain/LogSilenceAnalysis.hpp
#pragma once

#include "domain/ThresholdPolicy.hpp"

#include <expected>

namespace monitor {

struct LogSilencePolicy {
    std::string eqp_ip;
    std::uint32_t timeout_seconds{300};

    bool operator==(const LogSilencePolicy&) const = default;
};

struct LastLogReceipt {
    std::string eqp_ip;
    std::optional<std::chrono::system_clock::time_point> last_received_at;
};

struct LogReceiptBatch {
    std::chrono::system_clock::time_point observed_at;
    std::vector<LastLogReceipt> records;
};

enum class LogSilenceReadStatus { pending, ready, error };

struct LogSilenceReading {
    LogSilenceReadStatus status{LogSilenceReadStatus::pending};
    std::optional<std::chrono::system_clock::time_point> queried_at;
    std::optional<LogReceiptBatch> batch;
    std::optional<Error> error;
};

enum class LogSilenceStatus { disabled, pending, ok, warning, no_data, error };

struct LogSilenceEquipment {
    std::string eqp_ip;
    std::uint32_t timeout_seconds{};
    std::optional<std::chrono::system_clock::time_point> last_received_at;
    std::optional<std::uint64_t> silent_for_seconds;
    MetricStatus status{MetricStatus::unknown};
    std::string reason;
};

struct LogSilenceAnalysis {
    LogSilenceStatus status{LogSilenceStatus::disabled};
    std::uint32_t default_timeout_seconds{300};
    std::optional<std::chrono::system_clock::time_point> checked_at;
    std::optional<std::chrono::system_clock::time_point> queried_at;
    std::vector<LogSilenceEquipment> equipment;
    std::optional<Error> error;
};

std::expected<LogSilenceAnalysis, Error> analyze_log_silence(
    const LogReceiptBatch& batch, std::uint32_t default_timeout_seconds,
    const std::vector<LogSilencePolicy>& overrides);
const char* to_string(LogSilenceStatus status) noexcept;

}
