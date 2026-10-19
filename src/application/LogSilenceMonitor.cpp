// src/application/LogSilenceMonitor.cpp
#include "application/LogSilenceMonitor.hpp"

#include <utility>

namespace monitor {

LogSilenceMonitor::LogSilenceMonitor(ILogSilenceReader& reader,
    const IMonitorSettingsProvider& settings) : reader_(reader), settings_(settings) {}

void LogSilenceMonitor::settings_changed(std::chrono::system_clock::time_point) {
    const auto& settings = settings_.current();
    reader_.read_last_received(settings.is_analyzer, settings.collection_interval);
}

LogSilenceAnalysis LogSilenceMonitor::collect(std::chrono::system_clock::time_point) {
    const auto& settings = settings_.current();
    auto reading = reader_.read_last_received(settings.is_analyzer, settings.collection_interval);
    LogSilenceAnalysis result;
    result.default_timeout_seconds = settings.log_silence.default_timeout_seconds;
    if (!settings.is_analyzer) return result;
    result.queried_at = reading.queried_at;
    if (reading.status == LogSilenceReadStatus::error || reading.error) {
        result.status = LogSilenceStatus::error;
        result.error = reading.error.value_or(Error{"log_silence", "receipts", "query_failed",
            "Last log receipt times could not be read"});
        return result;
    }
    if (reading.status != LogSilenceReadStatus::ready) {
        result.status = LogSilenceStatus::pending;
        return result;
    }
    if (!reading.batch) {
        result.status = LogSilenceStatus::error;
        result.error = Error{"log_silence", "receipts", "invalid_reading",
            "Last log receipt results are missing the observation time and records"};
        return result;
    }
    auto evaluated = analyze_log_silence(*reading.batch, settings.log_silence.default_timeout_seconds,
        settings.log_silence.overrides);
    if (!evaluated) {
        result.status = LogSilenceStatus::error;
        result.error = evaluated.error();
        return result;
    }
    evaluated->queried_at = reading.queried_at;
    return std::move(*evaluated);
}

}
