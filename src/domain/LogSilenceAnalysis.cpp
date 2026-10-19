// src/domain/LogSilenceAnalysis.cpp
#include "domain/LogSilenceAnalysis.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace monitor {
namespace {

bool valid_equipment(const std::string& equipment) {
    return !equipment.empty() && equipment.size() <= 39
        && std::none_of(equipment.begin(), equipment.end(), [](unsigned char value) {
            return value < 32 || value == 127;
        });
}

bool valid_timeout(std::uint32_t timeout) {
    return timeout >= 60 && timeout <= 604800;
}

}

std::expected<LogSilenceAnalysis, Error> analyze_log_silence(
    const LogReceiptBatch& batch, std::uint32_t default_timeout_seconds,
    const std::vector<LogSilencePolicy>& overrides) {
    if (!valid_timeout(default_timeout_seconds)) {
        return std::unexpected(Error{"log_silence", "policies", "invalid_policy",
            "The default log silence timeout must be between 60 and 604800 seconds"});
    }
    std::unordered_map<std::string_view, std::uint32_t> timeouts;
    timeouts.reserve(overrides.size());
    for (const auto& policy : overrides) {
        if (!valid_equipment(policy.eqp_ip) || !valid_timeout(policy.timeout_seconds)
            || !timeouts.emplace(policy.eqp_ip, policy.timeout_seconds).second) {
            return std::unexpected(Error{"log_silence", "policies", "invalid_policy",
                "Log silence policies must contain unique equipment and valid timeouts"});
        }
    }
    std::unordered_set<std::string_view> equipment;
    equipment.reserve(batch.records.size());
    for (const auto& record : batch.records) {
        if (!valid_equipment(record.eqp_ip) || !equipment.insert(record.eqp_ip).second) {
            return std::unexpected(Error{"log_silence", "receipts", "invalid_batch",
                "Log receipt results must contain unique valid equipment"});
        }
    }

    LogSilenceAnalysis result;
    result.default_timeout_seconds = default_timeout_seconds;
    result.status = batch.records.empty() ? LogSilenceStatus::no_data : LogSilenceStatus::ok;
    result.checked_at = batch.observed_at;
    result.equipment.reserve(batch.records.size());
    for (const auto& record : batch.records) {
        LogSilenceEquipment entry;
        entry.eqp_ip = record.eqp_ip;
        const auto timeout = timeouts.find(record.eqp_ip);
        entry.timeout_seconds = timeout == timeouts.end() ? default_timeout_seconds : timeout->second;
        entry.last_received_at = record.last_received_at;
        if (!entry.last_received_at) {
            entry.status = MetricStatus::warning;
            entry.reason = "no_received_data";
        } else if (*entry.last_received_at > batch.observed_at) {
            entry.status = MetricStatus::unknown;
            entry.reason = "future_received_time";
            result.error = Error{"log_silence", "receipt_time", "future_received_time",
                "A log receipt timestamp is later than the observation time"};
        } else {
            const auto observed = std::chrono::duration<long double>(
                batch.observed_at.time_since_epoch()).count();
            const auto received = std::chrono::duration<long double>(
                entry.last_received_at->time_since_epoch()).count();
            const auto age = std::max(0.0L, std::floor(observed - received) - 60.0L);
            if (age > static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
                return std::unexpected(Error{"log_silence", "receipt_time", "invalid_received_time",
                    "A log receipt age cannot be represented"});
            }
            entry.silent_for_seconds = static_cast<std::uint64_t>(age);
            entry.status = *entry.silent_for_seconds >= entry.timeout_seconds
                ? MetricStatus::warning : MetricStatus::ok;
            entry.reason = entry.status == MetricStatus::warning
                ? "timeout_exceeded" : "receiving";
        }
        if (entry.status == MetricStatus::warning) {
            result.status = LogSilenceStatus::warning;
        } else if (entry.status == MetricStatus::unknown && result.status != LogSilenceStatus::warning) {
            result.status = LogSilenceStatus::error;
        }
        result.equipment.push_back(std::move(entry));
    }
    return result;
}

const char* to_string(LogSilenceStatus status) noexcept {
    switch (status) {
        case LogSilenceStatus::disabled: return "disabled";
        case LogSilenceStatus::pending: return "pending";
        case LogSilenceStatus::ok: return "ok";
        case LogSilenceStatus::warning: return "warning";
        case LogSilenceStatus::no_data: return "no_data";
        case LogSilenceStatus::error: return "error";
    }
    return "error";
}

}
