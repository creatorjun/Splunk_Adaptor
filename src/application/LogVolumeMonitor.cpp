// src/application/LogVolumeMonitor.cpp
#include "application/LogVolumeMonitor.hpp"
#include "application/LogCountPolicy.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace monitor {
namespace {

using Clock = std::chrono::system_clock;

bool same_error(const std::optional<Error>& left, const std::optional<Error>& right) {
    if (left.has_value() != right.has_value()) return false;
    if (!left) return true;
    return std::tie(left->component, left->target, left->code, left->message) ==
        std::tie(right->component, right->target, right->code, right->message);
}

bool same_result(const LogVolumeAnalysis& left, const LogVolumeAnalysis& right) {
    return left.status == right.status && left.window == right.window && left.queried_at == right.queried_at &&
        left.last_saved_hour == right.last_saved_hour && left.completed_intervals == right.completed_intervals &&
        left.backlog == right.backlog && same_error(left.error, right.error);
}

std::string source_identity(const MySqlSettings& settings) {
    std::string result;
    for (const auto& field : {settings.host, std::to_string(settings.port), settings.database,
        settings.credentials_file}) {
        result += std::to_string(field.size()) + ":" + field;
    }
    return result;
}

Error invalid_data() {
    return {"log_volume", "aggregation", "invalid_interval_data",
        "Log counts contain an invalid interval, equipment identifier, duplicate, or overflowing total"};
}

}

LogVolumeMonitor::LogVolumeMonitor(ILogVolumeReader& reader, const IMonitorSettingsProvider& settings,
    const ILogVolumeCalendar& calendar, ILogCountStore& store)
    : reader_(reader), settings_(settings), calendar_(calendar), store_(store) {}

bool LogVolumeMonitor::publish(LogVolumeAnalysis next) {
    if (next.error && next.error->component == "log_count_retention") {
        next.error.reset();
        next.status = active_ ? LogVolumeStatus::pending : status_before_retention_error_;
    }
    if (retention_error_ && !next.error) {
        status_before_retention_error_ = next.status;
        next.error = retention_error_;
        next.status = LogVolumeStatus::error;
    }
    current_ = std::move(next);
    if (current_.status == LogVolumeStatus::pending) return false;
    if (last_emitted_ && same_result(*last_emitted_, current_)) return false;
    last_emitted_ = current_;
    return true;
}

bool LogVolumeMonitor::fail(Error error) {
    auto next = current_;
    next.status = LogVolumeStatus::error;
    next.error = std::move(error);
    return publish(std::move(next));
}

std::expected<LogVolumeMonitor::PendingCommit, Error> LogVolumeMonitor::aggregate(
    const LogVolumeReading& reading, Clock::time_point timestamp) const {
    if (!state_ || !active_ || reading.window != active_ || state_->next_start != active_->start) {
        return std::unexpected(invalid_data());
    }
    PendingCommit candidate;
    candidate.state = {state_->source_identity, state_->next_start, std::nullopt, state_->last_saved_hour};
    if (state_->hour) {
        const auto& hour = *state_->hour;
        candidate.state.hour = LogCountHour{hour.start, hour.end, hour.coverage_start, hour.completed_intervals, {}};
    }
    candidate.analysis.status = reading.equipment.empty() ? LogVolumeStatus::no_data : LogVolumeStatus::ok;
    candidate.analysis.window = reading.window;
    candidate.analysis.queried_at = reading.queried_at;
    std::map<std::uint64_t, std::vector<const EquipmentLogVolume*>> intervals;
    std::set<std::pair<std::uint64_t, std::string_view>> unique;
    for (const auto& equipment : reading.equipment) {
        if (equipment.eqp_ip.empty() || equipment.eqp_ip.size() > 39 ||
            std::any_of(equipment.eqp_ip.begin(), equipment.eqp_ip.end(),
                [](unsigned char value) { return value < 32 || value == 127; }) ||
            !unique.emplace(equipment.start_bucket, equipment.eqp_ip).second) return std::unexpected(invalid_data());
        intervals[equipment.start_bucket].push_back(&equipment);
    }
    std::map<std::string, std::uint64_t> totals;
    if (state_->hour) {
        for (const auto& equipment : state_->hour->equipment) {
            if (!totals.emplace(equipment.eqp_ip, equipment.count).second) return std::unexpected(invalid_data());
        }
    }
    for (auto start = active_->start; start < active_->end; start += LogCountPolicy::query_interval) {
        const auto subwindow = calendar_.range(start, start + LogCountPolicy::query_interval);
        if (!subwindow) return std::unexpected(subwindow.error());
        if (!candidate.state.hour) {
            const auto hour = calendar_.hour(start);
            if (!hour) return std::unexpected(hour.error());
            candidate.state.hour = *hour;
            candidate.state.hour->coverage_start = start;
        }
        auto& hour = *candidate.state.hour;
        if (start < hour.start || subwindow->end > hour.end || hour.completed_intervals >= 12 ||
            hour.coverage_start + LogCountPolicy::query_interval * hour.completed_intervals != start) {
            return std::unexpected(invalid_data());
        }
        if (const auto found = intervals.find(subwindow->start_bucket); found != intervals.end()) {
            for (const auto* equipment : found->second) {
                auto& count = totals[equipment->eqp_ip];
                if (equipment->count > std::numeric_limits<std::uint64_t>::max() - count) {
                    return std::unexpected(invalid_data());
                }
                count += equipment->count;
            }
            intervals.erase(found);
        }
        if (totals.size() > 10000) return std::unexpected(invalid_data());
        ++hour.completed_intervals;
        candidate.state.next_start = subwindow->end;
        if (subwindow->end == hour.end || subwindow->end == active_->end) {
            hour.equipment.clear();
            hour.equipment.reserve(totals.size());
            for (const auto& [equipment, count] : totals) hour.equipment.push_back({equipment, count, 0});
        }
        if (subwindow->end == hour.end) {
            candidate.state.last_saved_hour = hour.start;
            candidate.reports.push_back({candidate.state.source_identity, std::move(hour), timestamp});
            candidate.state.hour.reset();
            totals.clear();
        }
    }
    if (!intervals.empty()) return std::unexpected(invalid_data());
    candidate.analysis.last_saved_hour = candidate.state.last_saved_hour;
    candidate.analysis.completed_intervals = candidate.state.hour ? candidate.state.hour->completed_intervals : 0;
    return candidate;
}

bool LogVolumeMonitor::tick(Clock::time_point timestamp) {
    const auto& settings = settings_.current();
    if (settings.role != ServerRole::anal) {
        current_ = {};
        last_emitted_.reset();
        active_.reset();
        return false;
    }
    const auto latest = calendar_.window(timestamp);
    if (!latest) return fail(latest.error());
    const auto identity = source_identity(settings.mysql);
    if (!state_ || state_->source_identity != identity) {
        auto loaded = store_.load(settings.output_directory);
        if (!loaded) return fail(loaded.error());
        LogCountState candidate = loaded->has_value() && (**loaded).source_identity == identity
            ? std::move(**loaded) : LogCountState{identity, latest->start, std::nullopt, std::nullopt};
        const auto saved = store_.commit(settings.output_directory, candidate, {});
        if (!saved) return fail(saved.error());
        state_ = std::move(candidate);
        directory_ = settings.output_directory;
        active_.reset();
        pending_.reset();
        next_attempt_.reset();
        next_prune_.reset();
        retention_error_.reset();
        current_ = {};
        current_.status = state_->next_start >= latest->end ? LogVolumeStatus::ok : LogVolumeStatus::pending;
        current_.last_saved_hour = state_->last_saved_hour;
        current_.completed_intervals = state_->hour ? state_->hour->completed_intervals : 0;
    }
    if (directory_ != settings.output_directory) {
        const auto moved = store_.commit(settings.output_directory, *state_, {});
        if (!moved) return fail(moved.error());
        directory_ = settings.output_directory;
        next_prune_.reset();
    }
    if (current_.status == LogVolumeStatus::disabled) {
        current_.status = state_->next_start >= latest->end ? LogVolumeStatus::ok : LogVolumeStatus::pending;
        current_.last_saved_hour = state_->last_saved_hour;
        current_.completed_intervals = state_->hour ? state_->hour->completed_intervals : 0;
        current_.backlog = state_->next_start < latest->start;
    }
    bool maintenance_changed = false;
    if (!next_prune_ || timestamp >= *next_prune_) {
        const bool had_retention_error = retention_error_.has_value();
        const auto pruned = store_.prune(directory_, timestamp - LogCountPolicy::retention);
        next_prune_ = timestamp + std::chrono::minutes{5};
        if (!pruned) {
            retention_error_ = pruned.error();
            retention_error_->component = "log_count_retention";
        } else {
            retention_error_.reset();
        }
        if (had_retention_error || retention_error_) maintenance_changed = publish(current_);
    }
    if (pending_) {
        const auto committed = store_.commit(directory_, pending_->state, pending_->reports);
        if (!committed) return fail(committed.error());
        state_ = std::move(pending_->state);
        auto next = std::move(pending_->analysis);
        next.backlog = state_->next_start < latest->end;
        pending_.reset();
        active_.reset();
        next_attempt_ = latest->end + LogCountPolicy::query_interval;
        return publish(std::move(next));
    }
    if (!active_) {
        if (state_->next_start >= latest->end) return maintenance_changed;
        if (next_attempt_ && timestamp < *next_attempt_) return maintenance_changed;
        auto end = latest->end;
        const auto lag = std::chrono::duration_cast<std::chrono::seconds>(end.time_since_epoch()) -
            std::chrono::duration_cast<std::chrono::seconds>(state_->next_start.time_since_epoch());
        if (lag > LogCountPolicy::maximum_query_range) end = state_->next_start + LogCountPolicy::maximum_query_range;
        const auto desired = calendar_.range(state_->next_start, end);
        if (!desired) return fail(desired.error());
        active_ = *desired;
        next_attempt_ = latest->end + LogCountPolicy::query_interval;
        current_.status = retention_error_ ? LogVolumeStatus::error : LogVolumeStatus::pending;
        status_before_retention_error_ = LogVolumeStatus::pending;
        current_.error = retention_error_;
        current_.window = active_;
        current_.queried_at.reset();
        current_.backlog = active_->end < latest->end;
    }
    auto reading = reader_.read_log_volume(*active_);
    if (reading.window != active_ || reading.status == LogVolumeReadStatus::pending) return maintenance_changed;
    if (reading.status == LogVolumeReadStatus::error || reading.error) {
        current_.queried_at = reading.queried_at;
        return fail(reading.error.value_or(Error{"log_volume", "query", "query_failed",
            "Log count query failed; the checkpoint was not advanced"}));
    }
    auto candidate = aggregate(reading, timestamp);
    if (!candidate) {
        active_.reset();
        return fail(candidate.error());
    }
    pending_ = std::move(*candidate);
    const auto committed = store_.commit(directory_, pending_->state, pending_->reports);
    if (!committed) return fail(committed.error());
    state_ = std::move(pending_->state);
    auto next = std::move(pending_->analysis);
    next.backlog = state_->next_start < latest->end;
    pending_.reset();
    active_.reset();
    next_attempt_ = latest->end + LogCountPolicy::query_interval;
    return publish(std::move(next));
}

LogVolumeAnalysis LogVolumeMonitor::collect() {
    return current_;
}

}
