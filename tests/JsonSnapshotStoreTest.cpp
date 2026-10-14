// tests/JsonSnapshotStoreTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/persistence/JsonSnapshotStore.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

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

}

int main() {
    return test::run([] {
        TemporaryDirectory directory;
        monitor::JsonSnapshotStore store(directory.path);
        auto snapshot = make_snapshot();
        CHECK(store.save(snapshot));
        const auto path = directory.path / "20261014T123400Z.json";
        const auto json = read_json(path);
        CHECK(json["schema_version"] == 1);
        CHECK(json["timestamp"] == "2026-10-14T12:34:00Z");
        CHECK(json["thresholds_applied_at"] == "2026-10-14T12:33:59.877Z");
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
        CHECK(read_json(directory.path / "20261014T123400Z_1.json")["hostname"] == "second-host");

        std::atomic<unsigned> successful{};
        std::vector<std::jthread> writers;
        for (unsigned index = 0; index < 8; ++index) {
            writers.emplace_back([&] { if (store.save(snapshot)) ++successful; });
        }
        writers.clear();
        CHECK(successful == 8);
        std::size_t entries = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory.path)) {
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
        const auto invalid_utf8 = read_json(directory.path / "20261014T123401Z.json");
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
        CHECK(std::filesystem::is_empty(failed_directory));
    });
}
