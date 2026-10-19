// tests/LogSilenceAnalysisTest.cpp
#include "TestSupport.hpp"
#include "domain/LogSilenceAnalysis.hpp"

#include <limits>

namespace {

using namespace monitor;
using namespace std::chrono_literals;

const auto observed = std::chrono::system_clock::time_point{2000s};

void threshold_and_precision_tests() {
    const LogReceiptBatch batch{observed, {
        {"device-a", observed - 359s}, {"device-b", observed - 360s},
        {"device-c", observed - 361s}, {"device-d", observed - 359999ms},
        {"device-e", observed - 360001ms}, {"device-f", observed - 60s},
        {"device-g", observed - 59999ms}, {"device-h", observed}}};
    const auto result = analyze_log_silence(batch, 300, {});
    CHECK(result);
    CHECK(result->status == LogSilenceStatus::warning);
    CHECK(result->default_timeout_seconds == 300);
    CHECK(result->checked_at == observed);
    CHECK(!result->queried_at);
    CHECK(!result->error);
    CHECK(result->equipment.size() == 8);
    CHECK(result->equipment[0].silent_for_seconds == 299);
    CHECK(result->equipment[0].status == MetricStatus::ok);
    CHECK(result->equipment[0].reason == "receiving");
    CHECK(result->equipment[1].silent_for_seconds == 300);
    CHECK(result->equipment[1].status == MetricStatus::warning);
    CHECK(result->equipment[1].reason == "timeout_exceeded");
    CHECK(result->equipment[1].last_received_at == observed - 360s);
    CHECK(result->equipment[2].silent_for_seconds == 301);
    CHECK(result->equipment[2].status == MetricStatus::warning);
    CHECK(result->equipment[3].silent_for_seconds == 299);
    CHECK(result->equipment[3].status == MetricStatus::ok);
    CHECK(result->equipment[4].silent_for_seconds == 300);
    CHECK(result->equipment[4].status == MetricStatus::warning);
    for (std::size_t index = 5; index < result->equipment.size(); ++index) {
        CHECK(result->equipment[index].silent_for_seconds == 0);
        CHECK(result->equipment[index].status == MetricStatus::ok);
    }
    const auto minimum = analyze_log_silence({observed, {{"device-a", observed - 120s}}}, 60, {});
    CHECK(minimum->equipment.front().silent_for_seconds == 60);
    CHECK(minimum->status == LogSilenceStatus::warning);
    const auto maximum = analyze_log_silence({observed, {{"device-a", observed - 604860s}}}, 604800, {});
    CHECK(maximum->equipment.front().silent_for_seconds == 604800);
    CHECK(maximum->status == LogSilenceStatus::warning);
}

void dynamic_equipment_and_override_tests() {
    const LogReceiptBatch batch{observed, {
        {"device-a", observed - 360s}, {"device-b", observed - 360s},
        {"device-c", observed - 120s}}};
    const std::vector<LogSilencePolicy> overrides{{"device-b", 600}, {"device-c", 60},
        {"device-not-in-database", 60}};
    const auto result = analyze_log_silence(batch, 300, overrides);
    CHECK(result);
    CHECK(result->equipment.size() == 3);
    CHECK(result->equipment[0].eqp_ip == "device-a");
    CHECK(result->equipment[0].timeout_seconds == 300);
    CHECK(result->equipment[0].status == MetricStatus::warning);
    CHECK(result->equipment[1].timeout_seconds == 600);
    CHECK(result->equipment[1].status == MetricStatus::ok);
    CHECK(result->equipment[2].timeout_seconds == 60);
    CHECK(result->equipment[2].status == MetricStatus::warning);
    const auto changed = analyze_log_silence({observed + 1s,
        {{"new-device", observed}}}, 300, overrides);
    CHECK(changed);
    CHECK(changed->equipment.size() == 1);
    CHECK(changed->equipment.front().eqp_ip == "new-device");
    CHECK(changed->equipment.front().timeout_seconds == 300);
    CHECK(changed->status == LogSilenceStatus::ok);
    const auto empty = analyze_log_silence({observed, {}}, 300, overrides);
    CHECK(empty);
    CHECK(empty->status == LogSilenceStatus::no_data);
    CHECK(empty->equipment.empty());
    CHECK(empty->checked_at == observed);
}

void missing_future_and_summary_tests() {
    const auto missing = analyze_log_silence({observed, {{"device-a", std::nullopt}}}, 300, {});
    CHECK(missing);
    CHECK(missing->status == LogSilenceStatus::warning);
    CHECK(missing->equipment.front().status == MetricStatus::warning);
    CHECK(missing->equipment.front().reason == "no_received_data");
    CHECK(!missing->equipment.front().last_received_at);
    CHECK(!missing->equipment.front().silent_for_seconds);
    CHECK(!missing->error);
    const auto future = analyze_log_silence({observed, {{"device-a", observed + 1ns}}}, 300, {});
    CHECK(future);
    CHECK(future->status == LogSilenceStatus::error);
    CHECK(future->equipment.front().status == MetricStatus::unknown);
    CHECK(future->equipment.front().reason == "future_received_time");
    CHECK(future->equipment.front().last_received_at == observed + 1ns);
    CHECK(!future->equipment.front().silent_for_seconds);
    CHECK(future->error->code == "future_received_time");
    const auto warning_first = analyze_log_silence({observed,
        {{"device-a", observed - 360s}, {"device-b", observed + 1s}}}, 300, {});
    CHECK(warning_first->status == LogSilenceStatus::warning);
    CHECK(warning_first->error);
    const auto warning_last = analyze_log_silence({observed,
        {{"device-b", observed + 1s}, {"device-a", observed - 360s}}}, 300, {});
    CHECK(warning_last->status == LogSilenceStatus::warning);
    CHECK(warning_last->error);
    const auto error_last_ok = analyze_log_silence({observed,
        {{"device-b", observed + 1s}, {"device-a", observed}}}, 300, {});
    CHECK(error_last_ok->status == LogSilenceStatus::error);
    const auto wide = analyze_log_silence({std::chrono::system_clock::time_point::max(),
        {{"device-a", std::chrono::system_clock::time_point::min()}}}, 300, {});
    CHECK(wide);
    CHECK(wide->status == LogSilenceStatus::warning);
    CHECK(*wide->equipment.front().silent_for_seconds > 300);
}

void invalid_input_tests() {
    for (const auto timeout : {0U, 59U, 604801U, std::numeric_limits<std::uint32_t>::max()}) {
        const auto invalid_default = analyze_log_silence({observed, {}}, timeout, {});
        CHECK(!invalid_default);
        CHECK(invalid_default.error().code == "invalid_policy");
        const auto invalid_override = analyze_log_silence({observed, {}}, 300, {{"device-a", timeout}});
        CHECK(!invalid_override);
        CHECK(invalid_override.error().code == "invalid_policy");
    }
    const std::string invalid_equipment[]{"", std::string(40, 'a'), "device-a\n",
        std::string("device-a\0", 9), "device-a\x7f"};
    for (const auto& equipment : invalid_equipment) {
        const auto invalid_override = analyze_log_silence({observed, {}}, 300, {{equipment, 300}});
        CHECK(!invalid_override);
        CHECK(invalid_override.error().code == "invalid_policy");
        const auto invalid_batch = analyze_log_silence({observed, {{equipment, observed}}}, 300, {});
        CHECK(!invalid_batch);
        CHECK(invalid_batch.error().code == "invalid_batch");
    }
    const auto duplicate_override = analyze_log_silence({observed, {}}, 300,
        {{"device-a", 300}, {"device-a", 600}});
    CHECK(!duplicate_override);
    CHECK(duplicate_override.error().code == "invalid_policy");
    const auto duplicate_batch = analyze_log_silence({observed,
        {{"device-a", observed}, {"device-a", observed - 360s}}}, 300, {});
    CHECK(!duplicate_batch);
    CHECK(duplicate_batch.error().code == "invalid_batch");
    CHECK(duplicate_batch.error().target.find("device-a") == std::string::npos);
    CHECK(duplicate_batch.error().message.find("device-a") == std::string::npos);
}

void status_tests() {
    CHECK(std::string(to_string(LogSilenceStatus::disabled)) == "disabled");
    CHECK(std::string(to_string(LogSilenceStatus::pending)) == "pending");
    CHECK(std::string(to_string(LogSilenceStatus::ok)) == "ok");
    CHECK(std::string(to_string(LogSilenceStatus::warning)) == "warning");
    CHECK(std::string(to_string(LogSilenceStatus::no_data)) == "no_data");
    CHECK(std::string(to_string(LogSilenceStatus::error)) == "error");
    CHECK(std::string(to_string(static_cast<LogSilenceStatus>(-1))) == "error");
}

}

int main() {
    return test::run([] {
        threshold_and_precision_tests();
        dynamic_equipment_and_override_tests();
        missing_future_and_summary_tests();
        invalid_input_tests();
        status_tests();
    });
}
