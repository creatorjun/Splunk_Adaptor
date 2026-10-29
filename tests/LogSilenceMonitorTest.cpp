// tests/LogSilenceMonitorTest.cpp
#include "TestSupport.hpp"
#include "application/LogSilenceMonitor.hpp"

namespace {

using namespace monitor;
using namespace std::chrono_literals;

struct Settings final : IMonitorSettingsProvider {
    MonitorSettings value{};
    const MonitorSettings& current() const override { return value; }
};

struct Reader final : ILogSilenceReader {
    LogSilenceReading next;
    unsigned reads{};
    unsigned cancellations{};
    bool enabled{};
    std::chrono::seconds interval{};
    LogSilenceReading read_last_received(bool value, std::chrono::seconds period) override {
        ++reads;
        if (!value) ++cancellations;
        enabled = value;
        interval = period;
        return next;
    }
};

void role_and_reload_tests() {
    Settings settings;
    Reader reader;
    LogSilenceMonitor monitor{reader, settings};
    const auto now = std::chrono::system_clock::time_point{2000s};
    settings.value.log_silence.default_timeout_seconds = 600;
    auto result = monitor.collect(now);
    CHECK(result.status == LogSilenceStatus::disabled);
    CHECK(result.default_timeout_seconds == 600);
    CHECK(reader.reads == 1);
    CHECK(reader.cancellations == 1);
    CHECK(!reader.enabled);
    monitor.settings_changed(now);
    CHECK(reader.cancellations == 2);
    settings.value.role = ServerRole::analyzer;
    result = monitor.collect(now);
    CHECK(result.status == LogSilenceStatus::pending);
    CHECK(result.default_timeout_seconds == 600);
    CHECK(reader.enabled);
    CHECK(reader.interval == 60s);
    settings.value.collection_interval = 5s;
    monitor.settings_changed(now);
    CHECK(reader.enabled);
    CHECK(reader.interval == 5s);
    reader.next.status = LogSilenceReadStatus::ready;
    reader.next.queried_at = now + 1s;
    reader.next.batch = LogReceiptBatch{now, {{"device-a", now - 360s}}};
    result = monitor.collect(now + 10000s);
    CHECK(result.status == LogSilenceStatus::ok);
    CHECK(result.checked_at == now);
    CHECK(result.queried_at == now + 1s);
    CHECK(result.equipment.front().silent_for_seconds == 300);
    CHECK(result.equipment.front().timeout_seconds == 600);
    settings.value.log_silence.overrides = {{"device-a", 300}, {"not-in-database", 60}};
    monitor.settings_changed(now + 10001s);
    result = monitor.collect(now + 10002s);
    CHECK(result.status == LogSilenceStatus::warning);
    CHECK(result.equipment.size() == 1);
    CHECK(result.equipment.front().timeout_seconds == 300);
    CHECK(result.equipment.front().silent_for_seconds == 300);
    settings.value.log_silence.overrides.clear();
    settings.value.log_silence.default_timeout_seconds = 301;
    result = monitor.collect(now + 10003s);
    CHECK(result.status == LogSilenceStatus::ok);
    CHECK(result.default_timeout_seconds == 301);
    CHECK(result.equipment.front().timeout_seconds == 301);
    reader.next.batch = LogReceiptBatch{now + 1s, {{"new-device", now}}};
    result = monitor.collect(now + 10004s);
    CHECK(result.equipment.size() == 1);
    CHECK(result.equipment.front().eqp_ip == "new-device");
    CHECK(result.equipment.front().timeout_seconds == 301);
    reader.next.batch = LogReceiptBatch{now + 2s, {}};
    result = monitor.collect(now + 10005s);
    CHECK(result.status == LogSilenceStatus::no_data);
    CHECK(result.equipment.empty());
    CHECK(result.checked_at == now + 2s);
    CHECK(reader.enabled);
    settings.value.role = ServerRole::collector;
    reader.next.error = Error{"log_silence", "receipts", "query_failed", "Query failed"};
    result = monitor.collect(now);
    CHECK(result.status == LogSilenceStatus::disabled);
    CHECK(result.default_timeout_seconds == 301);
    CHECK(!result.error);
    CHECK(!result.queried_at);
    CHECK(!reader.enabled);
    CHECK(reader.cancellations == 3);
    monitor.settings_changed(now);
    CHECK(reader.cancellations == 4);
    settings.value.role = ServerRole::relay;
    monitor.settings_changed(now);
    result = monitor.collect(now);
    CHECK(result.status == LogSilenceStatus::disabled);
    CHECK(!result.error && !result.queried_at && result.equipment.empty());
    CHECK(!reader.enabled);
    CHECK(reader.cancellations == 6);
}

void pending_and_error_tests() {
    Settings settings;
    settings.value.role = ServerRole::analyzer;
    settings.value.log_silence.default_timeout_seconds = 600;
    Reader reader;
    LogSilenceMonitor monitor{reader, settings};
    const auto now = std::chrono::system_clock::time_point{2000s};
    reader.next.queried_at = now;
    reader.next.batch = LogReceiptBatch{now - 10000s, {{"device-a", now - 20000s}}};
    auto result = monitor.collect(now);
    CHECK(result.status == LogSilenceStatus::pending);
    CHECK(result.default_timeout_seconds == 600);
    CHECK(result.queried_at == now);
    CHECK(!result.checked_at);
    CHECK(result.equipment.empty());
    reader.next.status = LogSilenceReadStatus::error;
    result = monitor.collect(now);
    CHECK(result.status == LogSilenceStatus::error);
    CHECK(result.default_timeout_seconds == 600);
    CHECK(result.error->code == "query_failed");
    reader.next.error = Error{"log_silence", "receipts", "denied", "Query denied"};
    reader.next.status = LogSilenceReadStatus::ready;
    result = monitor.collect(now);
    CHECK(result.status == LogSilenceStatus::error);
    CHECK(result.error->code == "denied");
    CHECK(result.equipment.empty());
    reader.next.error.reset();
    reader.next.batch.reset();
    result = monitor.collect(now);
    CHECK(result.status == LogSilenceStatus::error);
    CHECK(result.error->code == "invalid_reading");
    reader.next.batch = LogReceiptBatch{now, {{"device-a", now}, {"device-a", now}}};
    result = monitor.collect(now);
    CHECK(result.status == LogSilenceStatus::error);
    CHECK(result.error->code == "invalid_batch");
    CHECK(result.default_timeout_seconds == 600);
    CHECK(result.queried_at == now);
    reader.next.batch = LogReceiptBatch{now, {{"device-a", now + 1s}}};
    result = monitor.collect(now);
    CHECK(result.status == LogSilenceStatus::error);
    CHECK(result.error->code == "future_received_time");
    CHECK(result.checked_at == now);
    CHECK(result.queried_at == now);
    CHECK(result.equipment.front().status == MetricStatus::unknown);
}

}

int main() {
    return test::run([] {
        role_and_reload_tests();
        pending_and_error_tests();
    });
}
