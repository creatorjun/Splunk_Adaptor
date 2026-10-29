// tests/LogCountValidationTest.cpp
#include "TestSupport.hpp"
#include "domain/LogCountValidation.hpp"

namespace {

using namespace monitor;
using namespace std::chrono;

void state_and_hour_invariants() {
    const auto start = sys_days{year{2026}/10/29};
    LogCountHour hour{start, start + hours{1}, start, 11, {{"192.0.2.1", 110, {}}}};
    LogCountState state{"test-source", start + minutes{55}, hour, std::nullopt};
    CHECK(valid_log_count_state(state));
    CHECK(valid_log_count_hour(hour, false));
    CHECK(!valid_log_count_hour(hour, true));
    hour.completed_intervals = 12;
    CHECK(valid_log_count_hour(hour, true));
    CHECK(!valid_log_count_hour(hour, false));
    hour.coverage_start = start + minutes{55};
    hour.completed_intervals = 1;
    CHECK(valid_log_count_hour(hour, true));
    hour.completed_intervals = 2;
    CHECK(!valid_log_count_hour(hour, true));
    hour = *state.hour;
    hour.start += milliseconds{1};
    CHECK(!valid_log_count_hour(hour, false));
    state.next_start += seconds{1};
    CHECK(!valid_log_count_state(state));
    state.next_start -= seconds{1};
    state.hour->completed_intervals = 10;
    CHECK(!valid_log_count_state(state));
    state.hour->completed_intervals = 11;
    state.hour->equipment.push_back(state.hour->equipment.front());
    CHECK(!valid_log_count_state(state));
    state.hour->equipment.pop_back();
    state.hour->equipment.front().interval_start = start;
    CHECK(!valid_log_count_state(state));
    state.hour->equipment.front().interval_start = {};
    state.hour->equipment.front().eqp_ip += '\n';
    CHECK(!valid_log_count_state(state));
    state.hour->equipment.front().eqp_ip = "192.0.2.1";
    state.last_saved_hour = state.next_start;
    CHECK(!valid_log_count_state(state));
    state.last_saved_hour = start - hours{1} + milliseconds{1};
    CHECK(!valid_log_count_state(state));
    state.last_saved_hour = start - hours{1};
    CHECK(valid_log_count_state(state));
    state.source_identity.clear();
    CHECK(!valid_log_count_state(state));
    CHECK(!valid_log_count_boundary(system_clock::time_point::min()));
    CHECK(!valid_log_count_boundary(system_clock::time_point::max()));
}

}

int main() {
    return test::run(state_and_hour_invariants);
}
