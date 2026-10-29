// tests/LogVolumeMonitorTest.cpp
#include "TestSupport.hpp"
#include "application/LogVolumeMonitor.hpp"

#include <limits>

namespace {

using namespace monitor;
using namespace std::chrono_literals;
using Clock = std::chrono::system_clock;
const auto base = Clock::time_point{std::chrono::hours{500000}};

struct Settings final : IMonitorSettingsProvider {
    MonitorSettings value;
    const MonitorSettings& current() const override { return value; }
};

struct Calendar final : ILogVolumeCalendar {
    bool invalid{false};
    std::expected<LogVolumeWindow, Error> range(Clock::time_point start, Clock::time_point end) const override {
        if (invalid || start >= end || end - start > 1h || (end - start) % 300s != Clock::duration::zero()) {
            return std::unexpected(Error{"log_volume", "calendar", "invalid_window", "Invalid window"});
        }
        return LogVolumeWindow{start, end};
    }
    std::expected<LogVolumeWindow, Error> window(Clock::time_point now) const override {
        const auto end = Clock::time_point{std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()) / 300 * 300};
        return range(end - 300s, end);
    }
    std::expected<LogCountHour, Error> hour(Clock::time_point now) const override {
        const auto start = std::chrono::floor<std::chrono::hours>(now);
        return LogCountHour{start, start + 1h, now, 0, {}};
    }
};

struct Reader final : ILogVolumeReader {
    LogVolumeReadStatus status{LogVolumeReadStatus::ready};
    unsigned calls{};
    std::uint64_t count{10};
    std::vector<LogVolumeWindow> requests;
    std::optional<std::vector<EquipmentLogVolume>> custom_equipment;
    bool invalid_bucket{false};
    LogVolumeReading read_log_volume(const LogVolumeWindow& window) override {
        ++calls;
        requests.push_back(window);
        LogVolumeReading result{status, window, window.end + 1s, {}, std::nullopt};
        if (status == LogVolumeReadStatus::error) result.error = Error{"mysql", "query", "connection_failed", "Unavailable"};
        if (status == LogVolumeReadStatus::ready) {
            if (custom_equipment) {
                result.equipment = *custom_equipment;
                return result;
            }
            for (auto interval = window.start; interval < window.end; interval += 5min) {
                result.equipment.push_back({"192.0.2.1", count, invalid_bucket ? interval + 1min : interval});
                result.equipment.push_back({"192.0.2.2", 0, interval});
            }
        }
        return result;
    }
};

struct Store final : ILogCountStore {
    std::optional<LogCountState> state;
    std::vector<LogCountReport> reports;
    bool fail_commit{false};
    bool fail_load{false};
    bool fail_prune{false};
    unsigned commits{};
    unsigned prunes{};
    std::string last_directory;
    std::expected<std::optional<LogCountState>, Error> load(const std::string&) override {
        if (fail_load) return std::unexpected(Error{"log_count_store", "state", "invalid_checkpoint", "Invalid checkpoint"});
        return state;
    }
    std::expected<void, Error> commit(const std::string& directory, const LogCountState& candidate,
        const std::vector<LogCountReport>& completed) override {
        ++commits;
        if (fail_commit) return std::unexpected(Error{"log_count_store", "disk", "io_error", "Disk full"});
        last_directory = directory;
        state = candidate;
        reports.insert(reports.end(), completed.begin(), completed.end());
        return {};
    }
    std::expected<std::size_t, Error> prune(const std::string&, Clock::time_point) override {
        ++prunes;
        if (fail_prune) return std::unexpected(Error{"log_count_store", "old-file", "io_error", "Cannot remove expired file"});
        return 0;
    }
};

void schedule_and_partial_hour() {
    Settings settings;
    Calendar calendar;
    Reader reader;
    Store store;
    LogVolumeMonitor monitor{reader, settings, calendar, store};
    CHECK(!monitor.tick(base + 37min));
    settings.value.role = ServerRole::prx;
    CHECK(!monitor.tick(base + 37min));
    CHECK(reader.calls == 0 && store.commits == 0);
    settings.value.role = ServerRole::anal;
    CHECK(monitor.tick(base + 37min));
    CHECK(reader.calls == 1 && store.state->next_start == base + 35min);
    CHECK(store.state->hour->coverage_start == base + 30min);
    CHECK(monitor.collect().completed_intervals == 1);
    settings.value.role = ServerRole::col;
    CHECK(!monitor.tick(base + 37min + 1s));
    settings.value.role = ServerRole::anal;
    CHECK(!monitor.tick(base + 37min + 2s));
    CHECK(monitor.collect().status == LogVolumeStatus::ok && monitor.collect().completed_intervals == 1);
    CHECK(reader.calls == 1);
    CHECK(!monitor.tick(base + 39min));
    CHECK(reader.calls == 1);
    for (int minute = 40; minute <= 60; minute += 5) CHECK(monitor.tick(base + std::chrono::minutes{minute}));
    CHECK(reader.calls == 6 && store.reports.size() == 1);
    const auto& report = store.reports.front();
    CHECK(report.hour.start == base && report.hour.end == base + 1h);
    CHECK(report.hour.coverage_start == base + 30min && report.hour.completed_intervals == 6);
    CHECK(report.hour.equipment.size() == 2 && report.hour.equipment[0].count == 60);
    CHECK(report.hour.equipment[1].count == 0);
    CHECK(!store.state->hour && store.state->last_saved_hour == base);
    CHECK(monitor.collect().last_saved_hour == base && !monitor.collect().backlog);
}

void resume_and_bounded_catchup() {
    Settings settings;
    settings.value.role = ServerRole::anal;
    Calendar calendar;
    Reader reader;
    Store store;
    {
        LogVolumeMonitor initial{reader, settings, calendar, store};
        CHECK(initial.tick(base + 1h));
    }
    CHECK(store.state->next_start == base + 1h);
    const auto reads = reader.calls;
    LogVolumeMonitor restarted{reader, settings, calendar, store};
    CHECK(!restarted.tick(base + 1h + 1min));
    CHECK(reader.calls == reads && restarted.collect().status == LogVolumeStatus::ok);
    CHECK(restarted.tick(base + 3h));
    CHECK(reader.requests.back().start == base + 1h && reader.requests.back().end == base + 2h);
    CHECK(restarted.collect().backlog);
    CHECK(store.reports.back().hour.completed_intervals == 12 && store.reports.back().hour.equipment[0].count == 120);
    const auto catchup_reads = reader.calls;
    CHECK(!restarted.tick(base + 3h + 1s));
    CHECK(reader.calls == catchup_reads);
    CHECK(restarted.tick(base + 3h + 5min));
    CHECK(reader.requests.back().start == base + 2h && reader.requests.back().end == base + 3h);
    CHECK(!restarted.tick(base + 3h + 5min + 1s));
    settings.value.output_directory = "/new-output";
    CHECK(!restarted.tick(base + 3h + 5min + 2s));
    CHECK(store.last_directory == "/new-output");
    CHECK(store.state->next_start == base + 3h);
}

void failures_and_retry() {
    Settings settings;
    settings.value.role = ServerRole::anal;
    Calendar calendar;
    Reader reader;
    Store store;
    reader.status = LogVolumeReadStatus::pending;
    LogVolumeMonitor monitor{reader, settings, calendar, store};
    CHECK(!monitor.tick(base + 5min));
    CHECK(store.state->next_start == base);
    reader.status = LogVolumeReadStatus::error;
    CHECK(monitor.tick(base + 5min + 1s));
    CHECK(monitor.collect().status == LogVolumeStatus::error);
    CHECK(store.state->next_start == base);
    CHECK(!monitor.tick(base + 5min + 2s));
    reader.status = LogVolumeReadStatus::ready;
    store.fail_commit = true;
    CHECK(monitor.tick(base + 5min + 3s));
    CHECK(monitor.collect().error->component == "log_count_store");
    CHECK(store.state->next_start == base);
    const auto reads = reader.calls;
    CHECK(!monitor.tick(base + 5min + 4s));
    CHECK(reader.calls == reads);
    store.fail_commit = false;
    CHECK(monitor.tick(base + 5min + 5s));
    CHECK(reader.calls == reads && store.state->next_start == base + 5min);
    CHECK(store.state->hour->equipment[0].count == 10);
    CHECK(!monitor.tick(base + 9min));
    CHECK(reader.calls == reads);
    CHECK(monitor.tick(base + 10min));
    CHECK(store.state->hour->equipment[0].count == 20);
}

void overflow_invalid_and_identity() {
    Settings settings;
    settings.value.role = ServerRole::anal;
    Calendar calendar;
    Reader reader;
    Store store;
    reader.count = std::numeric_limits<std::uint64_t>::max();
    LogVolumeMonitor monitor{reader, settings, calendar, store};
    CHECK(monitor.tick(base + 5min));
    reader.count = 1;
    CHECK(monitor.tick(base + 10min));
    CHECK(monitor.collect().error->code == "invalid_interval_data");
    CHECK(store.state->next_start == base + 5min);
    settings.value.mysql.host = "192.0.2.10";
    reader.invalid_bucket = true;
    CHECK(monitor.tick(base + 15min));
    CHECK(monitor.collect().error->code == "invalid_interval_data");
    CHECK(store.state->next_start == base + 10min && !store.state->hour);
    reader.invalid_bucket = false;
    CHECK(monitor.tick(base + 20min));
    CHECK(store.state->hour->coverage_start == base + 10min);
    settings.value.role = ServerRole::col;
    CHECK(!monitor.tick(base + 25min));
    CHECK(monitor.collect().status == LogVolumeStatus::disabled);
    store.fail_load = true;
    Settings second_settings;
    second_settings.value.role = ServerRole::anal;
    LogVolumeMonitor invalid{reader, second_settings, calendar, store};
    const auto reads = reader.calls;
    CHECK(invalid.tick(base + 30min));
    CHECK(invalid.collect().error->code == "invalid_checkpoint");
    CHECK(reader.calls == reads);
}

void retention_failure_does_not_stop_queries() {
    Settings settings;
    settings.value.role = ServerRole::anal;
    Calendar calendar;
    Reader reader;
    Store store;
    store.fail_prune = true;
    LogVolumeMonitor monitor{reader, settings, calendar, store};
    CHECK(monitor.tick(base + 5min));
    CHECK(store.state->next_start == base + 5min);
    CHECK(monitor.collect().error && monitor.collect().error->component == "log_count_retention");
    CHECK(!monitor.tick(base + 5min + 1s));
    CHECK(store.prunes == 1 && reader.calls == 1);
    CHECK(monitor.tick(base + 10min));
    CHECK(store.state->next_start == base + 10min && reader.calls == 2 && store.prunes == 2);
    store.fail_prune = false;
    CHECK(monitor.tick(base + 15min));
    CHECK(store.state->next_start == base + 15min && !monitor.collect().error);
}

void catchup_hour_boundary_is_transactional() {
    Settings settings;
    settings.value.role = ServerRole::anal;
    Calendar calendar;
    Reader reader;
    Store store;
    LogVolumeMonitor monitor{reader, settings, calendar, store};
    CHECK(monitor.tick(base + 55min));
    const auto checkpoint = *store.state;
    const auto previous_bucket = calendar.range(base + 55min, base + 1h)->start;
    const auto next_bucket = calendar.range(base + 1h, base + 1h + 5min)->start;
    reader.custom_equipment = std::vector<EquipmentLogVolume>{
        {"192.0.2.1", 20, previous_bucket},
        {"192.0.2.3", std::numeric_limits<std::uint64_t>::max(), next_bucket},
        {"192.0.2.3", 1, next_bucket + 45min}};
    CHECK(monitor.tick(base + 1h + 50min));
    CHECK(monitor.collect().error->code == "invalid_interval_data");
    CHECK(*store.state == checkpoint && store.reports.empty());
    reader.custom_equipment = std::vector<EquipmentLogVolume>{
        {"192.0.2.1", 5, next_bucket + 45min}, {"192.0.2.1", 20, previous_bucket},
        {"192.0.2.3", 7, next_bucket}};
    CHECK(monitor.tick(base + 1h + 55min));
    CHECK(store.reports.size() == 1);
    CHECK(store.reports.front().hour.completed_intervals == 2);
    CHECK(store.reports.front().hour.equipment.front().count == 30);
    CHECK(store.state->hour && store.state->hour->completed_intervals == 11);
    CHECK(store.state->hour->equipment.size() == 2);
    CHECK(store.state->hour->equipment[0].eqp_ip == "192.0.2.1" && store.state->hour->equipment[0].count == 5);
    CHECK(store.state->hour->equipment[1].eqp_ip == "192.0.2.3" && store.state->hour->equipment[1].count == 7);
}

void retention_recovery_preserves_no_data_status() {
    Settings settings;
    settings.value.role = ServerRole::anal;
    Calendar calendar;
    Reader reader;
    reader.custom_equipment = std::vector<EquipmentLogVolume>{};
    Store store;
    store.fail_prune = true;
    LogVolumeMonitor monitor{reader, settings, calendar, store};
    CHECK(monitor.tick(base + 5min + 10s));
    CHECK(monitor.collect().status == LogVolumeStatus::error);
    CHECK(monitor.tick(base + 10min));
    CHECK(reader.calls == 2 && monitor.collect().status == LogVolumeStatus::error);
    store.fail_prune = false;
    CHECK(monitor.tick(base + 10min + 10s));
    CHECK(reader.calls == 2 && monitor.collect().status == LogVolumeStatus::no_data);
    CHECK(!monitor.collect().error);
}

void invalid_restored_state_is_rejected() {
    Settings settings;
    settings.value.role = ServerRole::anal;
    Calendar calendar;
    Reader reader;
    Store store;
    store.state = LogCountState{"invalid-source", base + 1s, std::nullopt, std::nullopt};
    LogVolumeMonitor monitor{reader, settings, calendar, store};
    CHECK(monitor.tick(base + 5min));
    CHECK(monitor.collect().status == LogVolumeStatus::error);
    CHECK(monitor.collect().error->code == "invalid_interval_data");
    CHECK(reader.calls == 0 && store.commits == 0 && store.prunes == 0);
    CHECK(store.state->next_start == base + 1s);
}

}

int main() {
    return test::run([] {
        schedule_and_partial_hour();
        resume_and_bounded_catchup();
        failures_and_retry();
        overflow_invalid_and_identity();
        retention_failure_does_not_stop_queries();
        catchup_hour_boundary_is_transactional();
        retention_recovery_preserves_no_data_status();
        invalid_restored_state_is_rejected();
    });
}
