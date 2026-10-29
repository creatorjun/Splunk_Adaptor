// tests/JsonSnapshotStoreTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/persistence/JsonSnapshotStore.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

class ScopedTimezone {
public:
    explicit ScopedTimezone(const char* zone_spec) {
        if (const auto* previous = std::getenv("TZ")) previous_ = previous;
        set(zone_spec);
        ::tzset();
    }
    ~ScopedTimezone() {
        if (previous_) {
            ::setenv("TZ", previous_->c_str(), 1);
        } else {
            ::unsetenv("TZ");
        }
        ::tzset();
    }
    ScopedTimezone(const ScopedTimezone&) = delete;
    ScopedTimezone& operator=(const ScopedTimezone&) = delete;
    void set(const char* zone_spec) {
        if (::setenv("TZ", zone_spec, 1) != 0) throw std::runtime_error("setenv failed");
    }

private:
    std::optional<std::string> previous_;
};

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/resource-monitor-store-test-XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        path = created;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    std::filesystem::path path;
};

void check_no_threshold_values(const nlohmann::json& value) {
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            CHECK(key != "applied_thresholds");
            CHECK(key != "threshold_percent");
            CHECK(key != "default_timeout_seconds");
            CHECK(key != "timeout_seconds");
            CHECK(key != "threshold");
            check_no_threshold_values(child);
        }
    } else if (value.is_array()) {
        for (const auto& child : value) check_no_threshold_values(child);
    }
}

nlohmann::json read_json(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    auto document = nlohmann::json::parse(input);
    check_no_threshold_values(document);
    return document;
}

std::size_t count_descriptors() {
    std::size_t count{};
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        static_cast<void>(entry);
        ++count;
    }
    return count;
}

void check_no_temporary_files(const std::filesystem::path& path) {
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        CHECK(entry.path().extension() != ".tmp");
    }
}

monitor::ResourceSnapshot make_snapshot() {
    monitor::ResourceSnapshot snapshot;
    snapshot.timestamp = std::chrono::sys_days(std::chrono::year(2026) / 10 / 14) +
        std::chrono::hours(12) + std::chrono::minutes(34);
    snapshot.thresholds_applied_at = snapshot.timestamp - std::chrono::milliseconds(123);
    snapshot.hostname = "oracle-host";
    snapshot.status = monitor::MetricStatus::warning;
    snapshot.collection_status = monitor::CollectionStatus::partial;
    snapshot.config_status = monitor::ConfigStatus::using_last_valid;
    snapshot.applied_thresholds = {80, 85, 90};
    snapshot.missed_intervals = 2;
    snapshot.cpu = {monitor::MetricStatus::warning, 60.25, 80, 1, 2};
    snapshot.memory = {monitor::MetricStatus::ok, 8589934592ULL, 4294967296ULL, 4294967296ULL, 50};
    monitor::FilesystemMetrics disk;
    disk.status = monitor::MetricStatus::ok;
    disk.device = "/dev/mapper/ol-root";
    disk.device_id = "253:0";
    disk.filesystem_type = "xfs";
    disk.mount_points = {"/", "/path with space/\"quote\"\\slash\nline"};
    disk.total_bytes = 107374182400ULL;
    disk.used_bytes = 64424509440ULL;
    disk.free_bytes = 42949672960ULL;
    disk.available_bytes = 42949672960ULL;
    disk.usage_percent = 60;
    snapshot.filesystems.push_back(disk);
    monitor::FilesystemMetrics failed;
    failed.device = "/dev/sdb1";
    failed.device_id = "8:17";
    failed.filesystem_type = "xfs";
    failed.mount_points = {"/data"};
    snapshot.filesystems.push_back(failed);
    snapshot.errors.push_back({"filesystem", "/data", "statvfs_failed", "Permission denied"});
    return snapshot;
}

void check_timezone_snapshots() {
    ScopedTimezone scoped_zone("UTC0");
    TemporaryDirectory directory;
    monitor::JsonSnapshotStore store(directory.path);
    auto snapshot = make_snapshot();
    snapshot.timestamp += std::chrono::hours(11);
    snapshot.thresholds_applied_at = snapshot.timestamp - std::chrono::milliseconds(123);
    CHECK(store.save(snapshot));
    CHECK(read_json(directory.path / "20261014T233400+0000.json")["timestamp"] ==
        "2026-10-14T23:34:00+00:00");

    scoped_zone.set("KST-9");
    CHECK(store.save(snapshot));
    const auto seoul = read_json(directory.path / "20261015T083400+0900.json");
    CHECK(seoul["timestamp"] == "2026-10-15T08:34:00+09:00");
    CHECK(seoul["thresholds_applied_at"] == "2026-10-15T08:33:59.877+09:00");
    CHECK(store.save(snapshot));
    CHECK(read_json(directory.path / "20261015T083400+0900_1.json") == seoul);

    scoped_zone.set("NST3:30");
    CHECK(store.save(snapshot));
    const auto negative_half_hour = read_json(directory.path / "20261014T200400-0330.json");
    CHECK(negative_half_hour["timestamp"] == "2026-10-14T20:04:00-03:30");
    CHECK(negative_half_hour["thresholds_applied_at"] == "2026-10-14T20:03:59.877-03:30");

    scoped_zone.set("EST5EDT,M3.2.0/2,M11.1.0/2");
    snapshot.timestamp = std::chrono::sys_days(std::chrono::year(2026) / 11 / 1) +
        std::chrono::hours(5) + std::chrono::minutes(30);
    snapshot.thresholds_applied_at = snapshot.timestamp - std::chrono::milliseconds(123);
    CHECK(store.save(snapshot));
    const auto daylight = read_json(directory.path / "20261101T013000-0400.json");
    CHECK(daylight["timestamp"] == "2026-11-01T01:30:00-04:00");
    CHECK(daylight["thresholds_applied_at"] == "2026-11-01T01:29:59.877-04:00");
    snapshot.timestamp += std::chrono::hours(1);
    CHECK(store.save(snapshot));
    const auto standard = read_json(directory.path / "20261101T013000-0500.json");
    CHECK(standard["timestamp"] == "2026-11-01T01:30:00-05:00");
    CHECK(standard["thresholds_applied_at"] == "2026-11-01T01:29:59.877-04:00");
    CHECK(!std::filesystem::exists(directory.path / "20261101T013000-0400_1.json"));
    CHECK(!std::filesystem::exists(directory.path / "20261101T013000-0500_1.json"));
    check_no_temporary_files(directory.path);
}

void check_process_snapshots() {
    ScopedTimezone scoped_zone("UTC0");
    TemporaryDirectory directory;
    monitor::JsonSnapshotStore store(directory.path);
    auto snapshot = make_snapshot();
    snapshot.java_processes.role = monitor::ServerRole::anal;
    snapshot.java_processes.status = monitor::MetricStatus::warning;
    snapshot.java_processes.apps = {{"SearchEngine", true}, {"AnalyzeEngine", false},
        {"pmsServer", std::nullopt}};
    CHECK(store.save(snapshot));
    const auto analyzer = read_json(directory.path / "20261014T123400+0000.json");
    CHECK(analyzer["process_profile"] == "anal");
    CHECK(analyzer["process_status"] == "warning");
    CHECK(analyzer["processes"].is_object());
    CHECK(analyzer["processes"].size() == 3);
    CHECK(analyzer["processes"]["SearchEngine"].is_boolean());
    CHECK(analyzer["processes"]["SearchEngine"] == true);
    CHECK(analyzer["processes"]["AnalyzeEngine"].is_boolean());
    CHECK(analyzer["processes"]["AnalyzeEngine"] == false);
    CHECK(analyzer["processes"]["pmsServer"].is_null());

    snapshot.timestamp += std::chrono::seconds(1);
    snapshot.java_processes.role = monitor::ServerRole::col;
    snapshot.java_processes.status = monitor::MetricStatus::ok;
    snapshot.java_processes.apps = {{"Reducer", true}, {"CloudESMCollector", true}};
    CHECK(store.save(snapshot));
    const auto collector = read_json(directory.path / "20261014T123401+0000.json");
    CHECK(collector["process_profile"] == "col");
    CHECK(collector["process_status"] == "ok");
    CHECK(collector["processes"].size() == 2);
    CHECK(collector["processes"]["Reducer"] == true);
    CHECK(collector["processes"]["CloudESMCollector"] == true);
    CHECK(!collector["processes"].contains("AnalyzeEngine"));
    check_no_temporary_files(directory.path);
}

void check_relay_log_snapshots() {
    ScopedTimezone scoped_zone("UTC0");
    TemporaryDirectory directory;
    monitor::JsonSnapshotStore store(directory.path);
    auto snapshot = make_snapshot();
    snapshot.java_processes.role = monitor::ServerRole::prx;
    snapshot.relay_log.status = monitor::RelayLogStatus::warning;
    snapshot.relay_log.path = "/CloudESM/logs/CollectorForAgent.log";
    snapshot.relay_log.checked_at = snapshot.timestamp;
    snapshot.relay_log.events = {{"2026-10-29 10:42:07.413", "Connection refused", "ERROR \"line\"\t\xFF", true}};
    snapshot.relay_log.backlog = true;
    snapshot.relay_log.errors = {{"relay_log", snapshot.relay_log.path, "read_failed", "Cannot read more"}};
    CHECK(store.save(snapshot));
    const auto document = read_json(directory.path / "20261014T123400+0000.json");
    CHECK(document["role"] == "prx" && document["process_profile"] == "prx");
    const auto& relay = document["relay_log"];
    CHECK(relay["status"] == "warning" && relay["source"] == snapshot.relay_log.path);
    CHECK(relay["error_count"] == 1 && relay["events"].size() == 1);
    CHECK(relay["events"][0]["timestamp"] == "2026-10-29 10:42:07.413");
    CHECK(relay["events"][0]["message"] == "Connection refused");
    CHECK(relay["events"][0]["truncated"] == true && relay["backlog"] == true);
    CHECK(relay["errors"][0]["code"] == "read_failed");
    CHECK(relay["checked_at"] == "2026-10-14T12:34:00.000+00:00");
    snapshot.timestamp += std::chrono::seconds{1};
    snapshot.relay_log = {};
    CHECK(store.save(snapshot));
    const auto disabled = read_json(directory.path / "20261014T123401+0000.json")["relay_log"];
    CHECK(disabled["status"] == "disabled" && disabled["events"].empty());
    CHECK(disabled["error_count"] == 0 && disabled["checked_at"].is_null());
}

void check_log_volume_snapshots() {
    ScopedTimezone scoped_zone("KST-9");
    TemporaryDirectory directory;
    monitor::JsonSnapshotStore store(directory.path);
    auto snapshot = make_snapshot();
    snapshot.log_volume.status = monitor::LogVolumeStatus::ok;
    snapshot.log_volume.window = monitor::LogVolumeWindow{202610142125ULL, 202610142130ULL,
        std::chrono::sys_days{std::chrono::year{2026}/10/14} + std::chrono::hours{12} + std::chrono::minutes{25},
        std::chrono::sys_days{std::chrono::year{2026}/10/14} + std::chrono::hours{12} + std::chrono::minutes{30}};
    snapshot.log_volume.queried_at = snapshot.timestamp;
    snapshot.log_volume.last_saved_hour = std::chrono::sys_days{std::chrono::year{2026}/10/14} + std::chrono::hours{11};
    snapshot.log_volume.completed_intervals = 6;
    snapshot.log_volume.backlog = true;
    CHECK(store.save(snapshot));
    const auto document = read_json(directory.path / "20261014T213400+0900.json");
    CHECK(!document.contains("collect_count") && !document.contains("log_silence"));
    const auto& volume = document["log_volume"];
    CHECK(volume["source"] == "CloudESM.SUM_COLLECT_INFO");
    CHECK(volume["status"] == "ok");
    CHECK(volume["window_start"] == "2026-10-14T21:25:00.000+09:00");
    CHECK(volume["window_end_exclusive"] == "2026-10-14T21:30:00.000+09:00");
    CHECK(volume["queried_at"] == "2026-10-14T21:34:00.000+09:00");
    CHECK(volume["last_saved_hour"] == "2026-10-14T20:00:00.000+09:00");
    CHECK(volume["completed_intervals"] == 6 && volume["backlog"] == true);
    CHECK(!volume.contains("mode") && !volume.contains("equipment") && !volume.contains("equipment_count"));
    CHECK(volume["error"].is_null());
    snapshot.log_volume = {};
    snapshot.log_volume.status = monitor::LogVolumeStatus::error;
    snapshot.log_volume.error = monitor::Error{"log_volume", "query", "2013", "Query failed"};
    CHECK(store.save(snapshot));
    const auto failure = read_json(directory.path / "20261014T213400+0900_1.json")["log_volume"];
    CHECK(failure["status"] == "error");
    CHECK(failure["window_start"].is_null() && failure["window_end_exclusive"].is_null());
    CHECK(failure["last_saved_hour"].is_null() && failure["completed_intervals"] == 0 && failure["backlog"] == false);
    CHECK(failure["queried_at"].is_null() && failure["error"]["code"] == "2013");
}

void check_processing_files_snapshots() {
    ScopedTimezone scoped_zone("KST-9");
    TemporaryDirectory directory;
    monitor::JsonSnapshotStore store(directory.path);
    auto snapshot = make_snapshot();
    snapshot.processing_files.directories = {
        {"/CloudESM/data/processing/collector", 99, 0, {}},
        {"/CloudESM/data/processing/normalize", 10, 99, {}}};
    for (const auto state : {monitor::ProcessingFilesStatus::disabled, monitor::ProcessingFilesStatus::pending,
        monitor::ProcessingFilesStatus::ok, monitor::ProcessingFilesStatus::partial,
        monitor::ProcessingFilesStatus::error}) {
        TemporaryDirectory omitted_directory;
        monitor::JsonSnapshotStore omitted_store(omitted_directory.path);
        snapshot.processing_files.status = state;
        CHECK(omitted_store.save(snapshot));
        const auto omitted = read_json(omitted_directory.path / "20261014T213400+0900.json");
        CHECK(!omitted.contains("processing_files"));
        CHECK(omitted["cpu"]["usage_percent"] == *snapshot.cpu.usage_percent);
        CHECK(omitted["memory"]["total_bytes"] == *snapshot.memory.total_bytes);
    }

    snapshot.processing_files.status = monitor::ProcessingFilesStatus::warning;
    snapshot.processing_files.scan_started_at = snapshot.timestamp - std::chrono::milliseconds{1234};
    snapshot.processing_files.scanned_at = snapshot.timestamp - std::chrono::milliseconds{123};
    snapshot.processing_files.alerts = {
        {"/CloudESM/data/processing/collector", ".tmp", 100, 100},
        {"/CloudESM/data/processing/normalize", ".done", 120, 100}};
    CHECK(store.save(snapshot));
    const auto document = read_json(directory.path / "20261014T213400+0900.json");
    const auto& processing = document["processing_files"];
    CHECK(processing.size() == 5);
    CHECK(processing["status"] == "warning");
    CHECK(processing["scan_interval_seconds"] == 60);
    CHECK(processing["scan_started_at"] == "2026-10-14T21:33:58.766+09:00");
    CHECK(processing["scanned_at"] == "2026-10-14T21:33:59.877+09:00");
    CHECK(!processing.contains("directories"));
    CHECK(processing["alerts"].size() == 2);
    CHECK(processing["alerts"][0].size() == 3);
    CHECK(processing["alerts"][0]["path"] == "/CloudESM/data/processing/collector");
    CHECK(processing["alerts"][0]["extension"] == ".tmp");
    CHECK(processing["alerts"][0]["count"] == 100);
    CHECK(processing["alerts"][0]["count"].is_number_unsigned());
    CHECK(!processing["alerts"][0].contains("threshold"));
    CHECK(processing["alerts"][1]["path"] == "/CloudESM/data/processing/normalize");
    CHECK(processing["alerts"][1]["extension"] == ".done");
    CHECK(processing["alerts"][1]["count"] == 120);
    CHECK(!processing["alerts"][1].contains("threshold"));

    snapshot.processing_files.scan_started_at.reset();
    snapshot.processing_files.scanned_at.reset();
    snapshot.processing_files.alerts = {{"/CloudESM/data/processing/collector", ".done", 0, 0}};
    CHECK(store.save(snapshot));
    const auto zero = read_json(directory.path / "20261014T213400+0900_1.json")["processing_files"];
    CHECK(zero["scan_started_at"].is_null());
    CHECK(zero["scanned_at"].is_null());
    CHECK(zero["alerts"].size() == 1);
    CHECK(zero["alerts"][0]["count"] == 0);
    CHECK(!zero["alerts"][0].contains("threshold"));

    snapshot.processing_files.alerts.clear();
    snapshot.processing_files.status = monitor::ProcessingFilesStatus::error;
    snapshot.errors.push_back({"processing_files", "/CloudESM/data/processing/collector",
        "scan_failed", "Directory unavailable"});
    CHECK(store.save(snapshot));
    const auto failed = read_json(directory.path / "20261014T213400+0900_2.json");
    CHECK(!failed.contains("processing_files"));
    CHECK(failed["errors"].back()["component"] == "processing_files");
    CHECK(failed["errors"].back()["code"] == "scan_failed");
    check_no_temporary_files(directory.path);
}

}

int main() {
    return test::run([] {
        ScopedTimezone scoped_zone("UTC0");
        TemporaryDirectory directory;
        monitor::JsonSnapshotStore store(directory.path);
        CHECK(std::filesystem::is_empty(directory.path));
        auto snapshot = make_snapshot();
        CHECK(store.save(snapshot));
        const auto path = directory.path / "20261014T123400+0000.json";
        const auto json = read_json(path);
        CHECK(json["schema_version"] == 1);
        CHECK(json["timestamp"] == "2026-10-14T12:34:00+00:00");
        CHECK(json["thresholds_applied_at"] == "2026-10-14T12:33:59.877+00:00");
        CHECK(json["hostname"] == "oracle-host");
        CHECK(json["status"] == "warning");
        CHECK(json["collection_status"] == "partial");
        CHECK(json["config_status"] == "using_last_valid");
        CHECK(json["missed_intervals"] == 2);
        CHECK(!json.contains("applied_thresholds"));
        CHECK(json["cpu"]["window_seconds"] == 60.25);
        CHECK(json["cpu"]["iowait_percent"] == 1);
        CHECK(json["cpu"]["steal_percent"] == 2);
        CHECK(json["memory"]["total_bytes"].is_number_unsigned());
        CHECK(json["memory"]["total_bytes"] == 8589934592ULL);
        CHECK(json["filesystems"].size() == 2);
        CHECK(json["filesystems"][0]["mount_points"][1] == snapshot.filesystems[0].mount_points[1]);
        CHECK(json["filesystems"][1]["status"] == "unknown");
        CHECK(json["filesystems"][1]["total_bytes"].is_null());
        CHECK(json["filesystems"][1]["usage_percent"].is_null());
        CHECK(json["errors"][0]["code"] == "statvfs_failed");
        CHECK(json["processes"].is_object());
        CHECK(json["processes"].empty());
        CHECK(!json.contains("collect_count"));
        CHECK(!json.contains("log_silence"));
        CHECK(json["process_profile"] == "col");
        CHECK(json["process_status"] == "unknown");

        snapshot.hostname = "second-host";
        CHECK(store.save(snapshot));
        CHECK(read_json(path)["hostname"] == "oracle-host");
        CHECK(read_json(directory.path / "20261014T123400+0000_1.json")["hostname"] == "second-host");

        std::atomic<unsigned> successful{};
        std::vector<std::jthread> writers;
        for (unsigned index = 0; index < 8; ++index) {
            writers.emplace_back([&] { if (store.save(snapshot)) ++successful; });
        }
        writers.clear();
        CHECK(successful == 8);
        std::size_t entries = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory.path)) {
            if (entry.path().filename() == ".resource-monitor.lock") continue;
            CHECK(entry.path().extension() == ".json");
            CHECK(read_json(entry.path())["schema_version"] == 1);
            ++entries;
        }
        CHECK(entries == 10);

        snapshot.timestamp += std::chrono::seconds(1);
        snapshot.filesystems[0].mount_points = {std::string("/invalid-") + static_cast<char>(0xff)};
        snapshot.cpu.usage_percent.reset();
        snapshot.cpu.status = monitor::MetricStatus::unknown;
        CHECK(store.save(snapshot));
        const auto invalid_utf8 = read_json(directory.path / "20261014T123401+0000.json");
        CHECK(invalid_utf8["filesystems"][0]["mount_points"][0] == "/invalid-\xef\xbf\xbd");
        CHECK(invalid_utf8["cpu"]["usage_percent"].is_null());

        monitor::JsonSnapshotStore missing(directory.path / "missing");
        CHECK(!missing.save(snapshot));
        monitor::JsonSnapshotStore regular_file(path);
        CHECK(!regular_file.save(snapshot));
        const auto link_path = directory.path / "directory-link";
        std::filesystem::create_directory_symlink(directory.path, link_path);
        monitor::JsonSnapshotStore symlink(link_path);
        CHECK(!symlink.save(snapshot));
        CHECK(read_json(path)["hostname"] == "oracle-host");

        const auto failed_directory = directory.path / "write-failure";
        std::filesystem::create_directory(failed_directory);
        const auto child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            const struct rlimit limit {32, 32};
            if (std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR || ::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
                ::_exit(2);
            }
            monitor::JsonSnapshotStore limited(failed_directory);
            const auto result = limited.save(snapshot);
            ::_exit(!result && result.error().component == "snapshot_store" ? 0 : 1);
        }
        int child_status{};
        pid_t waited;
        do {
            waited = ::waitpid(child, &child_status, 0);
        } while (waited < 0 && errno == EINTR);
        CHECK(waited == child);
        CHECK(WIFEXITED(child_status));
        CHECK(WEXITSTATUS(child_status) == 0);
        CHECK(std::filesystem::is_regular_file(failed_directory / ".resource-monitor.lock"));
        check_no_temporary_files(failed_directory);

        TemporaryDirectory first_directory;
        TemporaryDirectory second_directory;
        TemporaryDirectory blocked_directory;
        monitor::JsonSnapshotStore switching(first_directory.path);
        auto moving_snapshot = make_snapshot();
        CHECK(switching.configure_output(first_directory.path.string()));
        CHECK(switching.save(moving_snapshot));
        const auto initial_descriptors = count_descriptors();
        CHECK(switching.configure_output((first_directory.path / ".").string()));
        CHECK(count_descriptors() == initial_descriptors);
        CHECK(switching.save(moving_snapshot));
        CHECK(std::filesystem::is_regular_file(first_directory.path / "20261014T123400+0000_1.json"));
        CHECK(switching.configure_output(second_directory.path.string()));
        CHECK(count_descriptors() == initial_descriptors);
        moving_snapshot.timestamp += std::chrono::seconds(1);
        CHECK(switching.save(moving_snapshot));
        CHECK(std::filesystem::is_regular_file(second_directory.path / "20261014T123401+0000.json"));
        CHECK(!std::filesystem::exists(first_directory.path / "20261014T123401+0000.json"));
        {
            monitor::JsonSnapshotStore released(first_directory.path);
            CHECK(released.configure_output(first_directory.path.string()));
            monitor::JsonSnapshotStore blocked(blocked_directory.path);
            CHECK(blocked.configure_output(blocked_directory.path.string()));
            const auto active_descriptors = count_descriptors();
            for (unsigned attempt = 0; attempt < 32; ++attempt) {
                CHECK(!switching.configure_output((first_directory.path / "missing").string()));
                CHECK(!switching.configure_output(blocked_directory.path.string()));
                CHECK(count_descriptors() == active_descriptors);
            }
            moving_snapshot.timestamp += std::chrono::seconds(1);
            CHECK(switching.save(moving_snapshot));
            CHECK(std::filesystem::is_regular_file(second_directory.path / "20261014T123402+0000.json"));
            CHECK(!std::filesystem::exists(blocked_directory.path / "20261014T123402+0000.json"));
        }
        CHECK(switching.configure_output(blocked_directory.path.string()));
        moving_snapshot.timestamp += std::chrono::seconds(1);
        CHECK(switching.save(moving_snapshot));
        CHECK(std::filesystem::is_regular_file(blocked_directory.path / "20261014T123403+0000.json"));
        CHECK(!std::filesystem::exists(second_directory.path / "20261014T123403+0000.json"));
        CHECK(count_descriptors() == initial_descriptors);

        TemporaryDirectory invalid_directory;
        const auto invalid_lock = invalid_directory.path / ".resource-monitor.lock";
        const auto lock_target = invalid_directory.path / "lock-target";
        {
            std::ofstream file(lock_target);
            file << "preserve target";
        }
        std::filesystem::create_symlink(lock_target, invalid_lock);
        CHECK(!switching.configure_output(invalid_directory.path.string()));
        CHECK(std::filesystem::is_symlink(invalid_lock));
        std::filesystem::remove(invalid_lock);
        CHECK(::mkfifo(invalid_lock.c_str(), 0600) == 0);
        CHECK(!switching.configure_output(invalid_directory.path.string()));
        CHECK(std::filesystem::is_fifo(invalid_lock));
        std::filesystem::remove(invalid_lock);
        std::filesystem::create_directory(invalid_lock);
        CHECK(!switching.configure_output(invalid_directory.path.string()));
        CHECK(std::filesystem::is_directory(invalid_lock));
        std::filesystem::remove(invalid_lock);
        std::filesystem::create_hard_link(lock_target, invalid_lock);
        const auto invalid_result = switching.configure_output(invalid_directory.path.string());
        CHECK(!invalid_result);
        CHECK(invalid_result.error().code == "invalid_lock");
        CHECK(std::filesystem::hard_link_count(lock_target) == 2);
        CHECK(count_descriptors() == initial_descriptors);
        moving_snapshot.timestamp += std::chrono::seconds(1);
        CHECK(switching.save(moving_snapshot));
        CHECK(std::filesystem::is_regular_file(blocked_directory.path / "20261014T123404+0000.json"));
        CHECK(!std::filesystem::exists(invalid_directory.path / "20261014T123404+0000.json"));
        std::filesystem::remove(invalid_lock);
        CHECK(switching.configure_output(invalid_directory.path.string()));
        CHECK(switching.save(moving_snapshot));
        CHECK(std::filesystem::is_regular_file(invalid_directory.path / "20261014T123404+0000.json"));
        CHECK(count_descriptors() == initial_descriptors);

        const auto rollback_child = ::fork();
        CHECK(rollback_child >= 0);
        if (rollback_child == 0) {
            struct rlimit original_limit{};
            if (::getrlimit(RLIMIT_FSIZE, &original_limit) != 0) ::_exit(2);
            auto restricted_limit = original_limit;
            restricted_limit.rlim_cur = 32;
            if (std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR || ::setrlimit(RLIMIT_FSIZE, &restricted_limit) != 0) {
                ::_exit(2);
            }
            const auto rejected = switching.configure_output(failed_directory.string());
            const auto alias_rejected = switching.configure_output((invalid_directory.path / ".").string());
            const auto write_rejected = switching.save(moving_snapshot);
            const bool original_path_kept = !write_rejected &&
                write_rejected.error().target == invalid_directory.path.string();
            if (::setrlimit(RLIMIT_FSIZE, &original_limit) != 0) ::_exit(2);
            moving_snapshot.timestamp += std::chrono::seconds(1);
            const auto saved = switching.save(moving_snapshot);
            ::_exit(!rejected && !alias_rejected && original_path_kept && saved &&
                count_descriptors() == initial_descriptors ? 0 : 1);
        }
        do {
            waited = ::waitpid(rollback_child, &child_status, 0);
        } while (waited < 0 && errno == EINTR);
        CHECK(waited == rollback_child);
        CHECK(WIFEXITED(child_status));
        CHECK(WEXITSTATUS(child_status) == 0);
        CHECK(std::filesystem::is_regular_file(invalid_directory.path / "20261014T123405+0000.json"));
        CHECK(!std::filesystem::exists(failed_directory / "20261014T123405+0000.json"));
        check_no_temporary_files(failed_directory);

        TemporaryDirectory rename_parent;
        const auto named_directory = rename_parent.path / "current";
        const auto renamed_directory = rename_parent.path / "accepted";
        std::filesystem::create_directory(named_directory);
        {
            monitor::JsonSnapshotStore pinned(named_directory);
            CHECK(pinned.configure_output(named_directory.string()));
            std::filesystem::rename(named_directory, renamed_directory);
            std::filesystem::create_directory(named_directory);
            CHECK(pinned.save(moving_snapshot));
            CHECK(std::filesystem::is_regular_file(renamed_directory / "20261014T123404+0000.json"));
            CHECK(std::filesystem::is_empty(named_directory));
            CHECK(pinned.configure_output(renamed_directory.string()));
            CHECK(pinned.configure_output(named_directory.string()));
            CHECK(pinned.save(moving_snapshot));
            CHECK(std::filesystem::is_regular_file(named_directory / "20261014T123404+0000.json"));
            CHECK(!std::filesystem::exists(renamed_directory / "20261014T123404+0000_1.json"));
        }
        CHECK(count_descriptors() == initial_descriptors);
        check_no_temporary_files(first_directory.path);
        check_no_temporary_files(second_directory.path);
        check_no_temporary_files(blocked_directory.path);
        check_no_temporary_files(invalid_directory.path);
        check_timezone_snapshots();
        check_process_snapshots();
        check_log_volume_snapshots();
        check_relay_log_snapshots();
        check_processing_files_snapshots();
    });
}
