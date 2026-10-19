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

nlohmann::json read_json(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return nlohmann::json::parse(input);
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
        CHECK(json["applied_thresholds"]["cpu_usage_percent"] == 80);
        CHECK(json["applied_thresholds"]["memory_usage_percent"] == 85);
        CHECK(json["applied_thresholds"]["storage_usage_percent"] == 90);
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
    });
}
