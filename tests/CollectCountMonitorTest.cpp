// tests/CollectCountMonitorTest.cpp
#include "TestSupport.hpp"
#include "application/CollectCountMonitor.hpp"

namespace {

using namespace monitor;

struct Settings final : IMonitorSettingsProvider {
    MonitorSettings value{};
    const MonitorSettings& current() const override { return value; }
};

struct Calendar final : ILocalDayWindow {
    std::expected<DailyCountWindow, Error> window = DailyCountWindow{20261018, 20261017};
    mutable ComparisonDay requested{};
    std::expected<DailyCountWindow, Error> make(std::chrono::system_clock::time_point,
        ComparisonDay day) const override {
        requested = day;
        return window;
    }
};

struct Reader final : ICollectCountReader {
    CollectCountReading next;
    unsigned reads{};
    DailyCountWindow window;
    std::chrono::seconds interval{};
    CollectCountReading read_daily_counts(const DailyCountWindow& value, std::chrono::seconds period) override {
        ++reads;
        window = value;
        interval = period;
        return next;
    }
};

}

int main() {
    return test::run([] {
        Settings settings;
        Calendar calendar;
        Reader reader;
        CollectCountMonitor monitor{reader, settings, calendar};
        const auto now = std::chrono::system_clock::now();
        CHECK(monitor.collect(now).status == CollectCountStatus::disabled);
        CHECK(reader.reads == 0);
        settings.value.is_analyzer = true;
        CHECK(monitor.collect(now).status == CollectCountStatus::pending);
        CHECK(reader.reads == 1);
        CHECK(reader.window == *calendar.window);
        CHECK(reader.interval == std::chrono::seconds{60});
        reader.next.status = CollectCountReadStatus::ready;
        reader.next.queried_at = now;
        reader.next.counts = {{"10.0.0.1", 110, 100, true, true}};
        auto result = monitor.collect(now);
        CHECK(result.status == CollectCountStatus::warning);
        CHECK(result.alerts.size() == 1);
        CHECK(result.queried_at == now);
        settings.value.analyzer.collect_count_change_percent = 11;
        settings.value.analyzer.comparison_day = ComparisonDay::today;
        settings.value.collection_interval = std::chrono::seconds{5};
        result = monitor.collect(now);
        CHECK(result.status == CollectCountStatus::ok);
        CHECK(result.alerts.empty());
        CHECK(result.threshold_percent == 11);
        CHECK(calendar.requested == ComparisonDay::today);
        CHECK(reader.interval == std::chrono::seconds{5});
        settings.value.collection_interval = std::chrono::seconds{86400};
        monitor.settings_changed(now);
        CHECK(reader.interval == std::chrono::seconds{86400});
        CHECK(reader.window == *calendar.window);
        reader.next.counts.clear();
        CHECK(monitor.collect(now).status == CollectCountStatus::no_data);
        reader.next.status = CollectCountReadStatus::error;
        reader.next.error = Error{"collect_count", "table", "denied", "Query failed"};
        CHECK(monitor.collect(now).error->code == "denied");
        calendar.window = std::unexpected(Error{"collect_count", "calendar", "invalid_date", "Invalid date"});
        const auto reads = reader.reads;
        CHECK(monitor.collect(now).error->code == "invalid_date");
        CHECK(reader.reads == reads);
        monitor.settings_changed(now);
        CHECK(reader.reads == reads);
        settings.value.is_analyzer = false;
        CHECK(monitor.collect(now).status == CollectCountStatus::disabled);
        monitor.settings_changed(now);
        CHECK(reader.reads == reads);
    });
}
