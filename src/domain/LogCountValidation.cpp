// src/domain/LogCountValidation.cpp
#include "domain/LogCountValidation.hpp"

#include "domain/LogCountPolicy.hpp"

#include <algorithm>
#include <set>
#include <string_view>

namespace monitor {
namespace {

using Clock = std::chrono::system_clock;

std::int64_t epoch(Clock::time_point timestamp) noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(timestamp.time_since_epoch()).count();
}

}

bool valid_log_count_boundary(Clock::time_point timestamp) noexcept {
    return timestamp.time_since_epoch() % LogCountPolicy::query_interval == Clock::duration::zero();
}

bool valid_log_count_equipment(const std::vector<EquipmentLogVolume>& equipment) {
    if (equipment.size() > LogCountPolicy::maximum_equipment) return false;
    std::set<std::string_view> seen;
    for (const auto& item : equipment) {
        if (item.eqp_ip.empty() || item.eqp_ip.size() > 39 || item.interval_start != Clock::time_point{} ||
            std::any_of(item.eqp_ip.begin(), item.eqp_ip.end(), [](unsigned char value) { return value < 32 || value == 127; }) ||
            !seen.insert(item.eqp_ip).second) return false;
    }
    return true;
}

bool valid_log_count_hour(const LogCountHour& hour, bool completed) {
    if (!valid_log_count_boundary(hour.start) || !valid_log_count_boundary(hour.end) ||
        !valid_log_count_boundary(hour.coverage_start)) return false;
    const auto start = epoch(hour.start);
    const auto end = epoch(hour.end);
    const auto coverage = epoch(hour.coverage_start);
    const auto collected = coverage + static_cast<std::int64_t>(hour.completed_intervals) * LogCountPolicy::query_interval.count();
    return end - start == std::chrono::duration_cast<std::chrono::seconds>(LogCountPolicy::report_interval).count() &&
        coverage >= start && coverage < end && hour.completed_intervals > 0 &&
        hour.completed_intervals <= LogCountPolicy::intervals_per_hour &&
        (completed ? collected == end : collected < end) && valid_log_count_equipment(hour.equipment);
}

bool valid_log_count_state(const LogCountState& state) {
    if (state.source_identity.empty() || state.source_identity.size() > 8192 || !valid_log_count_boundary(state.next_start) ||
        (state.last_saved_hour && (!valid_log_count_boundary(*state.last_saved_hour) || *state.last_saved_hour >= state.next_start))) return false;
    return !state.hour || (valid_log_count_hour(*state.hour, false) &&
        state.hour->coverage_start + LogCountPolicy::query_interval * state.hour->completed_intervals == state.next_start);
}

}
