// tests/LogVolumeMonitorTest.cpp
#include "TestSupport.hpp"
#include "application/LogVolumeMonitor.hpp"

namespace {

using namespace monitor;
using namespace std::chrono_literals;

struct Settings final : IMonitorSettingsProvider {
    MonitorSettings value;
    const MonitorSettings& current() const override { return value; }
};

struct Calendar final : ILogVolumeCalendar {
    mutable LogVolumeSettings requested;
    mutable unsigned calls{};
    std::expected<LogVolumeWindow, Error> next = LogVolumeWindow{
        LogVolumeMode::interval, 202610291000ULL, 202610291005ULL,
        std::chrono::system_clock::time_point{1000s}, std::chrono::system_clock::time_point{1300s}};
    std::expected<LogVolumeWindow, Error> window(std::chrono::system_clock::time_point,
        const LogVolumeSettings& settings) const override {
        ++calls;
        requested = settings;
        return next;
    }
};

struct Reader final : ILogVolumeReader {
    LogVolumeReading next;
    LogVolumeWindow requested;
    unsigned calls{};
    LogVolumeReading read_log_volume(const LogVolumeWindow& window) override {
        ++calls;
        requested = window;
        return next;
    }
};

void state_and_duplicate_tests() {
    Settings settings;
    Calendar calendar;
    Reader reader;
    LogVolumeMonitor monitor{reader, settings, calendar};
    const auto now = std::chrono::system_clock::time_point{1400s};
    CHECK(!monitor.tick(now));
    CHECK(monitor.collect().status == LogVolumeStatus::disabled);
    CHECK(reader.calls == 0 && calendar.calls == 0);
    settings.value.role = ServerRole::analyzer;
    CHECK(!monitor.tick(now));
    CHECK(monitor.collect().status == LogVolumeStatus::pending);
    CHECK(reader.requested == *calendar.next);
    reader.next.status = LogVolumeReadStatus::ready;
    reader.next.window = *calendar.next;
    reader.next.queried_at = now;
    reader.next.equipment = {{"device-a", 500}, {"device-b", 0}};
    CHECK(monitor.tick(now));
    auto result = monitor.collect();
    CHECK(result.status == LogVolumeStatus::ok);
    CHECK(result.equipment.size() == 2 && result.equipment[0].count == 500 && result.equipment[1].count == 0);
    CHECK(result.queried_at == now);
    CHECK(result.window == reader.next.window);
    CHECK(!monitor.tick(now + 1s));
    CHECK(!monitor.tick(now + 60s));
    CHECK(monitor.collect().queried_at == now);
    reader.next.queried_at = now + 2s;
    CHECK(monitor.tick(now + 2s));
    reader.next.status = LogVolumeReadStatus::error;
    reader.next.error = Error{"log_volume", "table", "1045", "Access denied"};
    CHECK(monitor.tick(now + 3s));
    result = monitor.collect();
    CHECK(result.status == LogVolumeStatus::error && result.error->code == "1045");
    CHECK(result.equipment.empty());
    CHECK(!monitor.tick(now + 4s));
    reader.next.status = LogVolumeReadStatus::ready;
    reader.next.error.reset();
    reader.next.equipment.clear();
    CHECK(monitor.tick(now + 5s));
    CHECK(monitor.collect().status == LogVolumeStatus::no_data);
    CHECK(!monitor.tick(now + 6s));
    settings.value.role = ServerRole::collector;
    const auto reads = reader.calls;
    CHECK(!monitor.tick(now + 7s));
    result = monitor.collect();
    CHECK(result.status == LogVolumeStatus::disabled && result.equipment.empty());
    CHECK(!result.error && !result.window && !result.queried_at);
    CHECK(reader.calls == reads);
    settings.value.role = ServerRole::relay;
    CHECK(!monitor.tick(now + 8s));
    result = monitor.collect();
    CHECK(result.status == LogVolumeStatus::disabled && result.equipment.empty());
    CHECK(!result.error && !result.window && !result.queried_at);
    CHECK(reader.calls == reads);
}

void reload_and_window_tests() {
    Settings settings;
    settings.value.role = ServerRole::analyzer;
    Calendar calendar;
    Reader reader;
    LogVolumeMonitor monitor{reader, settings, calendar};
    const auto now = std::chrono::system_clock::time_point{1400s};
    reader.next = {LogVolumeReadStatus::ready, *calendar.next, now, {{"device-a", 20}}, std::nullopt};
    CHECK(monitor.tick(now));
    settings.value.log_volume.interval_seconds = 600;
    calendar.next->start_bucket = 202610290955ULL;
    calendar.next->start -= 300s;
    CHECK(!monitor.tick(now + 1s));
    CHECK(calendar.requested.interval_seconds == 600);
    CHECK(monitor.collect().status == LogVolumeStatus::pending);
    CHECK(monitor.collect().equipment.empty());
    CHECK(!monitor.collect().queried_at);
    reader.next.window = *calendar.next;
    CHECK(monitor.tick(now + 2s));
    CHECK(!monitor.tick(now + 3s));
    settings.value.log_volume.mode = LogVolumeMode::daily;
    settings.value.log_volume.daily_at_minutes = 20;
    calendar.next = LogVolumeWindow{LogVolumeMode::daily, 20261028, 20261029, now - 24h, now};
    CHECK(!monitor.tick(now + 4s));
    CHECK(calendar.requested.mode == LogVolumeMode::daily && calendar.requested.daily_at_minutes == 20);
    CHECK(monitor.collect().mode == LogVolumeMode::daily);
    reader.next.window = *calendar.next;
    CHECK(monitor.tick(now + 5s));
    CHECK(!monitor.tick(now + 6s));
    calendar.next = std::unexpected(Error{"log_volume", "calendar", "invalid_window", "Invalid window"});
    const auto reads = reader.calls;
    CHECK(monitor.tick(now + 7s));
    CHECK(monitor.collect().status == LogVolumeStatus::error);
    CHECK(monitor.collect().error->code == "invalid_window");
    CHECK(monitor.collect().equipment.empty());
    CHECK(!monitor.tick(now + 8s));
    CHECK(reader.calls == reads);
}

}

int main() {
    return test::run([] {
        state_and_duplicate_tests();
        reload_and_window_tests();
        CHECK(std::string{to_string(LogVolumeMode::interval)} == "interval");
        CHECK(std::string{to_string(LogVolumeMode::daily)} == "daily");
        CHECK(std::string{to_string(LogVolumeStatus::disabled)} == "disabled");
        CHECK(std::string{to_string(LogVolumeStatus::pending)} == "pending");
        CHECK(std::string{to_string(LogVolumeStatus::ok)} == "ok");
        CHECK(std::string{to_string(LogVolumeStatus::no_data)} == "no_data");
        CHECK(std::string{to_string(LogVolumeStatus::error)} == "error");
    });
}
