// tests/CollectCountAnalysisTest.cpp
#include "TestSupport.hpp"
#include "domain/CollectCountAnalysis.hpp"

#include <limits>
#include <utility>

namespace {

using namespace monitor;

DailyCountPair count(std::string address, std::uint64_t current, std::uint64_t previous,
    bool current_present = true, bool previous_present = true) {
    return {std::move(address), current, previous, current_present, previous_present};
}

void threshold_boundary_tests() {
    const auto result = analyze_collect_counts({count("10.1.0.1", 110, 100),
        count("10.1.0.2", 90, 100), count("10.1.0.3", 109, 100),
        count("10.1.0.4", 91, 100), count("10.1.0.5", 100, 100)}, 10.0);
    CHECK(result);
    CHECK(result->status == CollectCountStatus::warning);
    CHECK(result->equipment_count == 5);
    CHECK(result->threshold_percent == 10.0);
    CHECK(result->alerts.size() == 2);
    CHECK(result->alerts[0].eqp_ip == "10.1.0.1");
    CHECK(result->alerts[0].reason == "increase");
    CHECK(result->alerts[0].change_percent == 10.0);
    CHECK(result->alerts[1].eqp_ip == "10.1.0.2");
    CHECK(result->alerts[1].reason == "decrease");
    CHECK(result->alerts[1].change_percent == -10.0);
    const auto quiet = analyze_collect_counts({count("10.1.0.1", 109999, 100000),
        count("10.1.0.2", 90001, 100000)}, 10.0);
    CHECK(quiet);
    CHECK(quiet->status == CollectCountStatus::ok);
    CHECK(quiet->alerts.empty());
    CHECK(analyze_collect_counts({count("10.1.0.1", 201, 100)}, 101.0)
        ->alerts.front().change_percent == 101.0);
    CHECK(analyze_collect_counts({count("10.1.0.1", 10001, 1)}, 1000000.0)
        ->alerts.front().change_percent == 1000000.0);
}

void decimal_threshold_boundary_tests() {
    for (const auto difference : {1ULL, 3ULL, 100ULL}) {
        const auto threshold = static_cast<double>(difference) / 10.0;
        const auto boundary = analyze_collect_counts({
            count("10.1.0.1", 1000 + difference, 1000),
            count("10.1.0.2", 1000 - difference, 1000)}, threshold);
        CHECK(boundary);
        CHECK(boundary->status == CollectCountStatus::warning);
        CHECK(boundary->alerts.size() == 2);
        CHECK(boundary->alerts[0].change_percent == threshold);
        CHECK(boundary->alerts[1].change_percent == -threshold);
        const auto below = analyze_collect_counts({
            count("10.1.0.1", 1000000 + difference * 1000 - 1, 1000000),
            count("10.1.0.2", 1000000 - difference * 1000 + 1, 1000000)}, threshold);
        CHECK(below);
        CHECK(below->status == CollectCountStatus::ok);
        CHECK(below->alerts.empty());
        const auto higher_threshold = analyze_collect_counts({
            count("10.1.0.1", 1000 + difference, 1000)},
            std::nextafter(threshold, std::numeric_limits<double>::infinity()));
        CHECK(higher_threshold);
        CHECK(higher_threshold->alerts.empty());
    }
    const auto minimum_threshold = analyze_collect_counts({count("10.1.0.1", 101, 100)},
        std::numeric_limits<double>::denorm_min());
    CHECK(minimum_threshold);
    CHECK(minimum_threshold->alerts.size() == 1);
}

void zero_and_presence_tests() {
    const auto zero = analyze_collect_counts({count("10.1.0.1", 0, 0),
        count("10.1.0.2", 0, 0, true, false),
        count("10.1.0.3", 0, 0, false, true)}, 10.0);
    CHECK(zero);
    CHECK(zero->status == CollectCountStatus::ok);
    CHECK(zero->equipment_count == 3);
    CHECK(zero->alerts.empty());

    const auto changed = analyze_collect_counts({count("10.1.0.1", 1, 0),
        count("10.1.0.2", 1, 0, true, false),
        count("10.1.0.3", 0, 100, false, true),
        count("10.1.0.4", 0, 100)}, 10.0);
    CHECK(changed);
    CHECK(changed->alerts.size() == 4);
    CHECK(changed->status == CollectCountStatus::warning);
    CHECK(changed->alerts[0].reason == "baseline_zero");
    CHECK(!changed->alerts[0].change_percent);
    CHECK(changed->alerts[0].previous_present);
    CHECK(changed->alerts[1].reason == "baseline_zero");
    CHECK(!changed->alerts[1].change_percent);
    CHECK(changed->alerts[1].current_present);
    CHECK(!changed->alerts[1].previous_present);
    CHECK(changed->alerts[2].reason == "decrease");
    CHECK(changed->alerts[2].change_percent == -100.0);
    CHECK(!changed->alerts[2].current_present);
    CHECK(changed->alerts[2].previous_present);
    CHECK(changed->alerts[3].change_percent == -100.0);
    CHECK(changed->alerts[3].current_present);
    CHECK(analyze_collect_counts({count("10.1.0.1", 0, 100, false, true)}, 101.0)
        ->status == CollectCountStatus::ok);
    CHECK(analyze_collect_counts({count("10.1.0.1", 1, 0)}, 1000000.0)
        ->status == CollectCountStatus::warning);

    const auto empty = analyze_collect_counts({}, 10.0);
    CHECK(empty);
    CHECK(empty->status == CollectCountStatus::no_data);
    CHECK(empty->equipment_count == 0);
    CHECK(empty->alerts.empty());
}

void large_count_tests() {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto result = analyze_collect_counts({count("10.1.0.1", maximum - 1, maximum),
        count("10.1.0.2", maximum, maximum - 1)}, 0.000000000000000001);
    CHECK(result);
    CHECK(result->alerts.size() == 2);
    CHECK(result->alerts[0].current_count == maximum - 1);
    CHECK(result->alerts[0].previous_count == maximum);
    CHECK(*result->alerts[0].change_percent < 0.0);
    CHECK(*result->alerts[0].change_percent > -0.000000000000000006);
    CHECK(result->alerts[1].current_count == maximum);
    CHECK(result->alerts[1].previous_count == maximum - 1);
    CHECK(*result->alerts[1].change_percent > 0.0);
    const auto just_below = analyze_collect_counts({
        count("10.1.0.1", 1099999999999999999ULL, 1000000000000000000ULL),
        count("10.1.0.2", 900000000000000001ULL, 1000000000000000000ULL)}, 10.0);
    CHECK(just_below);
    CHECK(just_below->status == CollectCountStatus::ok);
    CHECK(just_below->alerts.empty());
    const auto extreme = analyze_collect_counts({count("10.1.0.1", maximum, 1),
        count("10.1.0.2", 0, maximum)}, 10.0);
    CHECK(extreme);
    CHECK(extreme->alerts.size() == 2);
    CHECK(std::isfinite(*extreme->alerts[0].change_percent));
    CHECK(*extreme->alerts[0].change_percent > 1000000.0);
    CHECK(extreme->alerts[1].change_percent == -100.0);
}

void invalid_input_tests() {
    const double invalid_thresholds[]{0.0, -1.0, 1000000.1,
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()};
    for (const auto threshold : invalid_thresholds) {
        const auto result = analyze_collect_counts({}, threshold);
        CHECK(!result);
        CHECK(result.error().code == "invalid_threshold");
    }
    const std::string invalid_addresses[]{"", std::string(40, 'a'), "10.1.0.1\n",
        std::string("10.1.0.1\0", 9), "10.1.0.1\x7f", "\t10.1.0.1"};
    for (const auto& address : invalid_addresses) {
        const auto result = analyze_collect_counts({count(address, 1, 1)}, 10.0);
        CHECK(!result);
        CHECK(result.error().code == "invalid_eqp_ip");
    }
    CHECK(analyze_collect_counts({count("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", 1, 1)},
        10.0));
    const auto duplicate = analyze_collect_counts({count("10.1.0.1", 1, 1),
        count("10.1.0.1", 2, 1)}, 10.0);
    CHECK(!duplicate);
    CHECK(duplicate.error().code == "duplicate_eqp_ip");
    const DailyCountPair invalid_pairs[]{count("10.1.0.1", 1, 0, false, true),
        count("10.1.0.1", 0, 1, true, false),
        count("10.1.0.1", 0, 0, false, false)};
    for (const auto& pair : invalid_pairs) {
        const auto result = analyze_collect_counts({pair}, 10.0);
        CHECK(!result);
        CHECK(result.error().code == "invalid_count_pair");
    }
}

void status_tests() {
    CHECK(std::string(to_string(CollectCountStatus::disabled)) == "disabled");
    CHECK(std::string(to_string(CollectCountStatus::pending)) == "pending");
    CHECK(std::string(to_string(CollectCountStatus::ok)) == "ok");
    CHECK(std::string(to_string(CollectCountStatus::warning)) == "warning");
    CHECK(std::string(to_string(CollectCountStatus::no_data)) == "no_data");
    CHECK(std::string(to_string(CollectCountStatus::error)) == "error");
    CHECK(std::string(to_string(static_cast<CollectCountStatus>(-1))) == "error");
}

}

int main() {
    return test::run([] {
        threshold_boundary_tests();
        decimal_threshold_boundary_tests();
        zero_and_presence_tests();
        large_count_tests();
        invalid_input_tests();
        status_tests();
    });
}
