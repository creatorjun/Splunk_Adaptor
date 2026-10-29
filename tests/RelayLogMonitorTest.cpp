// tests/RelayLogMonitorTest.cpp
#include "TestSupport.hpp"
#include "application/RelayLogMonitor.hpp"

namespace {

using namespace monitor;

struct Settings final : IMonitorSettingsProvider {
    MonitorSettings value;
    const MonitorSettings& current() const override { return value; }
};

struct Reader final : IRelayLogReader {
    bool enabled{};
    std::string path;
    unsigned acknowledgements{};
    RelayLogSnapshot next;
    RelayLogSnapshot read(bool active, const std::string& selected_path) override {
        enabled = active;
        path = selected_path;
        return next;
    }
    void acknowledge() noexcept override { ++acknowledgements; }
};

}

int main() {
    return test::run([] {
        Settings settings;
        Reader reader;
        RelayLogMonitor monitor{reader, settings};
        monitor.settings_changed();
        CHECK(!reader.enabled);
        CHECK(reader.path == "/CloudESM/logs/CollectorForAgent.log");
        settings.value.role = ServerRole::relay;
        monitor.settings_changed();
        CHECK(reader.enabled);
        reader.next.status = RelayLogStatus::warning;
        reader.next.events = {{"2026-10-29 10:42:07.413", "Connection refused", "ERROR line", false}};
        CHECK(monitor.collect().events.front().message == "Connection refused");
        CHECK(reader.acknowledgements == 0);
        monitor.acknowledge();
        CHECK(reader.acknowledgements == 1);
        settings.value.relay_log.path = "/another/CollectorForAgent.log";
        monitor.settings_changed();
        CHECK(reader.path == settings.value.relay_log.path);
        settings.value.role = ServerRole::analyzer;
        monitor.collect();
        CHECK(!reader.enabled);
        settings.value.role = ServerRole::collector;
        monitor.settings_changed();
        CHECK(!reader.enabled);
    });
}
