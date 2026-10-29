// tests/ProcessingFileMonitorTest.cpp
#include "TestSupport.hpp"
#include "application/ProcessingFileMonitor.hpp"

namespace {

using namespace monitor;

const std::string collector_path = "/CloudESM/data/processing/collector";
const std::string normalize_path = "/CloudESM/data/processing/normalize";

struct Settings final : IMonitorSettingsProvider {
    MonitorSettings value;
    const MonitorSettings& current() const override { return value; }
};

struct Reader final : IProcessingFileReader {
    ProcessingFilesSnapshot next;
    std::vector<bool> requests;
    bool was_enabled{};

    ProcessingFilesSnapshot read(bool enabled) override {
        requests.push_back(enabled);
        if (enabled && !was_enabled) {
            next = {};
            next.status = ProcessingFilesStatus::pending;
        }
        was_enabled = enabled;
        return next;
    }
};

}

int main() {
    return test::run([] {
        Settings settings;
        Reader reader;
        ProcessingFileMonitor monitor{reader, settings};
        auto snapshot = monitor.collect();
        CHECK(reader.requests == std::vector<bool>{true});
        CHECK(snapshot.status == ProcessingFilesStatus::pending);
        CHECK(snapshot.scan_interval_seconds == 60);
        CHECK(snapshot.directories.empty());

        const auto started = std::chrono::system_clock::time_point{std::chrono::seconds{100}};
        const auto finished = started + std::chrono::seconds{2};
        reader.next.status = ProcessingFilesStatus::ok;
        reader.next.scan_started_at = started;
        reader.next.scanned_at = finished;
        reader.next.directories = {{collector_path, 7, 12, {}}, {normalize_path, 0, 2, {}}};
        snapshot = monitor.collect();
        CHECK(snapshot.status == ProcessingFilesStatus::ok);
        CHECK(snapshot.scan_started_at == started);
        CHECK(snapshot.scanned_at == finished);
        CHECK(snapshot.directories.size() == 2);
        CHECK(snapshot.directories.front().path == collector_path);
        CHECK(snapshot.directories.front().tmp_count == 7);
        CHECK(snapshot.directories.front().done_count == 12);
        CHECK(snapshot.directories.back().tmp_count == 0);
        CHECK(snapshot.directories.back().done_count == 2);
        CHECK(snapshot.alerts.empty());

        reader.next.directories = {{collector_path, 100, 99, {}}, {normalize_path, 101, 100, {}}};
        snapshot = monitor.collect();
        CHECK(snapshot.status == ProcessingFilesStatus::warning);
        CHECK(snapshot.alerts.size() == 3);
        CHECK(snapshot.alerts[0].path == collector_path);
        CHECK(snapshot.alerts[0].extension == ".tmp");
        CHECK(snapshot.alerts[0].count == 100);
        CHECK(snapshot.alerts[0].threshold == 100);
        CHECK(snapshot.alerts[1].path == normalize_path);
        CHECK(snapshot.alerts[1].extension == ".tmp");
        CHECK(snapshot.alerts[1].count == 101);
        CHECK(snapshot.alerts[2].path == normalize_path);
        CHECK(snapshot.alerts[2].extension == ".done");
        CHECK(snapshot.alerts[2].count == 100);
        CHECK(reader.next.status == ProcessingFilesStatus::ok);
        CHECK(reader.next.alerts.empty());

        settings.value.processing_files.collector = {101, 99};
        settings.value.processing_files.normalize = {102, 101};
        monitor.settings_changed();
        snapshot = monitor.collect();
        CHECK(snapshot.status == ProcessingFilesStatus::warning);
        CHECK(snapshot.alerts.size() == 1);
        CHECK(snapshot.alerts.front().path == collector_path);
        CHECK(snapshot.alerts.front().extension == ".done");
        CHECK(snapshot.alerts.front().count == 99);
        CHECK(snapshot.alerts.front().threshold == 99);
        CHECK(snapshot.scan_started_at == started);
        CHECK(snapshot.scanned_at == finished);

        settings.value.processing_files.collector.done_count = 100;
        snapshot = monitor.collect();
        CHECK(snapshot.status == ProcessingFilesStatus::ok);
        CHECK(snapshot.alerts.empty());
        CHECK(snapshot.scanned_at == finished);

        settings.value.processing_files.collector = {0, 0};
        settings.value.processing_files.normalize = {0, 0};
        reader.next.directories = {{collector_path, 0, 0, {}}, {normalize_path, 0, 0, {}}};
        snapshot = monitor.collect();
        CHECK(snapshot.status == ProcessingFilesStatus::warning);
        CHECK(snapshot.alerts.size() == 4);
        for (const auto& alert : snapshot.alerts) {
            CHECK(alert.count == 0);
            CHECK(alert.threshold == 0);
        }

        settings.value.role = ServerRole::anal;
        monitor.settings_changed();
        CHECK(!reader.requests.back());
        snapshot = monitor.collect();
        CHECK(!reader.requests.back());
        CHECK(snapshot.status == ProcessingFilesStatus::disabled);
        CHECK(snapshot.directories.empty());
        CHECK(!snapshot.scan_started_at);
        CHECK(!snapshot.scanned_at);
        CHECK(snapshot.alerts.empty());

        settings.value.role = ServerRole::col;
        monitor.settings_changed();
        CHECK(reader.requests.back());
        snapshot = monitor.collect();
        CHECK(snapshot.status == ProcessingFilesStatus::pending);
        CHECK(snapshot.directories.empty());
        CHECK(!snapshot.scanned_at);

        reader.next.status = ProcessingFilesStatus::partial;
        reader.next.scan_started_at = started;
        reader.next.scanned_at = finished;
        settings.value.processing_files = {};
        reader.next.directories = {{collector_path, 4, 8, {}}, {normalize_path, {}, {},
            {{"processing_files", normalize_path, "permission_denied", "Cannot read directory"}}}};
        snapshot = monitor.collect();
        CHECK(snapshot.status == ProcessingFilesStatus::partial);
        CHECK(snapshot.directories.front().tmp_count == 4);
        CHECK(!snapshot.directories.back().tmp_count);
        CHECK(!snapshot.directories.back().done_count);
        CHECK(snapshot.directories.back().errors.size() == 1);
        CHECK(snapshot.directories.back().errors.front().code == "permission_denied");
        CHECK(snapshot.alerts.empty());

        settings.value.processing_files.collector = {4, 9};
        settings.value.processing_files.normalize = {0, 0};
        snapshot = monitor.collect();
        CHECK(snapshot.status == ProcessingFilesStatus::warning);
        CHECK(snapshot.alerts.size() == 1);
        CHECK(snapshot.alerts.front().path == collector_path);
        CHECK(snapshot.alerts.front().extension == ".tmp");
        CHECK(snapshot.directories.back().errors.front().code == "permission_denied");

        reader.next.directories.back().tmp_count = 200;
        reader.next.directories.back().done_count = 200;
        snapshot = monitor.collect();
        CHECK(snapshot.alerts.size() == 1);
        CHECK(snapshot.alerts.front().path == collector_path);
        reader.next.directories.back().tmp_count.reset();
        reader.next.directories.back().done_count.reset();

        reader.next.status = ProcessingFilesStatus::error;
        reader.next.directories.front().tmp_count.reset();
        reader.next.directories.front().done_count.reset();
        reader.next.directories.front().errors = {
            {"processing_files", collector_path, "not_found", "Directory is missing"}};
        snapshot = monitor.collect();
        CHECK(snapshot.status == ProcessingFilesStatus::error);
        CHECK(snapshot.scan_started_at == started);
        CHECK(snapshot.scanned_at == finished);
        CHECK(!snapshot.directories.front().tmp_count);
        CHECK(snapshot.directories.front().errors.front().code == "not_found");
        CHECK(snapshot.directories.back().errors.front().code == "permission_denied");
        CHECK(snapshot.alerts.empty());

        settings.value.role = ServerRole::anal;
        snapshot = monitor.collect();
        CHECK(!reader.requests.back());
        CHECK(snapshot.status == ProcessingFilesStatus::disabled);
        CHECK(snapshot.directories.empty());
        CHECK(!snapshot.scanned_at);

        Settings analyzer_settings;
        analyzer_settings.value.role = ServerRole::anal;
        Reader analyzer_reader;
        ProcessingFileMonitor analyzer{analyzer_reader, analyzer_settings};
        analyzer.settings_changed();
        CHECK(analyzer_reader.requests == std::vector<bool>{false});
        CHECK(analyzer.collect().status == ProcessingFilesStatus::disabled);
        CHECK(!analyzer_reader.was_enabled);
        analyzer_settings.value.role = ServerRole::prx;
        analyzer.settings_changed();
        CHECK(analyzer.collect().status == ProcessingFilesStatus::disabled);
        CHECK(!analyzer_reader.requests.back());
        CHECK(!analyzer_reader.was_enabled);

        CHECK(to_string(ProcessingFilesStatus::disabled) == "disabled");
        CHECK(to_string(ProcessingFilesStatus::pending) == "pending");
        CHECK(to_string(ProcessingFilesStatus::ok) == "ok");
        CHECK(to_string(ProcessingFilesStatus::partial) == "partial");
        CHECK(to_string(ProcessingFilesStatus::error) == "error");
        CHECK(to_string(ProcessingFilesStatus::warning) == "warning");
    });
}
