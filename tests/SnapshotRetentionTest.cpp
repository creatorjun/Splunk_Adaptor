// tests/SnapshotRetentionTest.cpp
#include "TestSupport.hpp"
#include "application/SnapshotRetention.hpp"
#include "infrastructure/persistence/JsonSnapshotStore.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        char pattern[] = "/tmp/resource-monitor-retention-test-XXXXXX";
        const auto* created = ::mkdtemp(pattern);
        if (!created) throw std::runtime_error("Unable to create retention fixture");
        path = created;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
            std::filesystem::perm_options::add, ignored);
        std::filesystem::remove_all(path, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    std::filesystem::path path;
};

class ScopedTimezone {
public:
    explicit ScopedTimezone(const char* zone) {
        if (const auto* previous = std::getenv("TZ")) previous_ = previous;
        set(zone);
    }
    ~ScopedTimezone() {
        if (previous_) ::setenv("TZ", previous_->c_str(), 1);
        else ::unsetenv("TZ");
        ::tzset();
    }
    ScopedTimezone(const ScopedTimezone&) = delete;
    ScopedTimezone& operator=(const ScopedTimezone&) = delete;
    void set(const char* zone) {
        CHECK(::setenv("TZ", zone, 1) == 0);
        ::tzset();
    }

private:
    std::optional<std::string> previous_;
};

std::chrono::system_clock::time_point base_time() {
    return std::chrono::sys_days(std::chrono::year{2026} / 10 / 27) + 10h;
}

monitor::ResourceSnapshot make_snapshot(std::chrono::system_clock::time_point time) {
    monitor::ResourceSnapshot snapshot;
    snapshot.timestamp = time;
    snapshot.thresholds_applied_at = time;
    snapshot.hostname = "retention-fixture";
    snapshot.status = monitor::MetricStatus::ok;
    snapshot.collection_status = monitor::CollectionStatus::ok;
    snapshot.config_status = monitor::ConfigStatus::ok;
    snapshot.applied_thresholds = {80, 85, 90};
    snapshot.cpu = {monitor::MetricStatus::ok, 60, 10, 0, 0};
    snapshot.memory = {monitor::MetricStatus::ok, 1000, 200, 800, 20};
    monitor::FilesystemMetrics filesystem;
    filesystem.status = monitor::MetricStatus::ok;
    filesystem.device = "/dev/fixture";
    filesystem.device_id = "1:1";
    filesystem.filesystem_type = "xfs";
    filesystem.mount_points = {"/"};
    filesystem.total_bytes = 1000;
    filesystem.used_bytes = 200;
    filesystem.free_bytes = 800;
    filesystem.available_bytes = 800;
    filesystem.usage_percent = 20;
    snapshot.filesystems.push_back(filesystem);
    return snapshot;
}

nlohmann::json read_json(const std::filesystem::path& path) {
    std::ifstream input(path);
    return nlohmann::json::parse(input);
}

void write_text(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary);
    output << value;
    output.close();
    CHECK(output.good());
}

void write_json(const std::filesystem::path& path, const nlohmann::json& value) {
    write_text(path, value.dump() + "\n");
}

std::size_t count_descriptors() {
    std::size_t result{};
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        static_cast<void>(entry);
        ++result;
    }
    return result;
}

std::size_t prune_count(monitor::JsonSnapshotStore& store, std::chrono::system_clock::time_point cutoff) {
    const auto result = store.prune(cutoff);
    CHECK(result);
    return *result;
}

void check_retention_boundary() {
    ScopedTimezone timezone("UTC0");
    TemporaryDirectory directory;
    monitor::JsonSnapshotStore store(directory.path);
    const auto now = base_time() + 2h;
    const auto cutoff = now - 1h;
    CHECK(store.save(make_snapshot(cutoff - 1s)));
    CHECK(store.save(make_snapshot(cutoff)));
    CHECK(store.save(make_snapshot(cutoff + 1s)));
    CHECK(store.save(make_snapshot(now + 1s)));
    const auto pruned = store.prune(cutoff);
    CHECK(pruned);
    CHECK(*pruned == 2);
    CHECK(!std::filesystem::exists(directory.path / "20261027T105959+0000.json"));
    CHECK(!std::filesystem::exists(directory.path / "20261027T110000+0000.json"));
    CHECK(std::filesystem::is_regular_file(directory.path / "20261027T110001+0000.json"));
    CHECK(std::filesystem::is_regular_file(directory.path / "20261027T120001+0000.json"));
    CHECK(prune_count(store, cutoff) == 0);
    const auto fractional = store.prune(cutoff + 999ms);
    CHECK(fractional);
    CHECK(*fractional == 0);
    CHECK(prune_count(store, cutoff + 1s) == 1);
    CHECK(std::filesystem::is_regular_file(directory.path / "20261027T120001+0000.json"));
    CHECK(std::filesystem::is_regular_file(directory.path / ".resource-monitor.lock"));
}

void check_configurable_retention_policy() {
    class RecordingRetention final : public monitor::ISnapshotRetention {
    public:
        std::expected<std::size_t, monitor::Error> prune(std::chrono::system_clock::time_point value) override {
            ++calls;
            cutoff = value;
            return 7;
        }
        unsigned calls{};
        std::chrono::system_clock::time_point cutoff;
    };
    RecordingRetention storage;
    monitor::SnapshotRetention policy(storage);
    monitor::SnapshotRetentionSettings settings;
    settings.enabled = false;
    const auto disabled = policy.run(std::chrono::system_clock::time_point::min(), settings);
    CHECK(disabled && *disabled == 0);
    CHECK(storage.calls == 0);
    settings.enabled = true;
    for (const auto age : {60s, 7200s, 31536000s}) {
        settings.max_age = age;
        const auto result = policy.run(base_time(), settings);
        CHECK(result && *result == 7);
        CHECK(storage.cutoff == base_time() - age);
    }
    CHECK(storage.calls == 3);
    for (const auto age : {-1s, 0s, 59s, 31536001s, std::chrono::seconds::max()}) {
        settings.max_age = age;
        const auto result = policy.run(base_time(), settings);
        CHECK(!result && result.error().code == "invalid_retention");
    }
    CHECK(storage.calls == 3);
    ScopedTimezone timezone("UTC0");
    TemporaryDirectory directory;
    monitor::JsonSnapshotStore store(directory.path);
    CHECK(store.save(make_snapshot(base_time())));
    CHECK(store.save(make_snapshot(base_time() + 1s)));
    monitor::SnapshotRetention cleanup(store);
    settings = {true, 2h};
    const auto before_boundary = cleanup.run(base_time() + 2h - 1s, settings);
    CHECK(before_boundary && *before_boundary == 0);
    const auto boundary = cleanup.run(base_time() + 2h, settings);
    CHECK(boundary && *boundary == 1);
    CHECK(std::filesystem::exists(directory.path / "20261027T100001+0000.json"));
    settings.enabled = false;
    const auto retained = cleanup.run(base_time() + 24h, settings);
    CHECK(retained && *retained == 0);
    CHECK(std::filesystem::exists(directory.path / "20261027T100001+0000.json"));
}

void check_offsets_legacy_and_collisions() {
    ScopedTimezone timezone("KST-9");
    TemporaryDirectory directory;
    monitor::JsonSnapshotStore store(directory.path);
    CHECK(store.save(make_snapshot(base_time())));
    CHECK(store.save(make_snapshot(base_time())));
    CHECK(store.save(make_snapshot(base_time() + 2h)));
    CHECK(std::filesystem::exists(directory.path / "20261027T190000+0900_1.json"));
    timezone.set("NST3:30");
    CHECK(store.save(make_snapshot(base_time())));
    CHECK(std::filesystem::exists(directory.path / "20261027T063000-0330.json"));
    timezone.set("UTC0");
    CHECK(store.save(make_snapshot(base_time())));
    const auto current = directory.path / "20261027T100000+0000.json";
    auto legacy = read_json(current);
    legacy["timestamp"] = "2026-10-27T10:00:00Z";
    write_json(current, legacy);
    std::filesystem::rename(current, directory.path / "20261027T100000Z.json");
    write_json(directory.path / "20261027T100000Z_9999.json", legacy);
    timezone.set("HST10");
    const auto pruned = store.prune(base_time() + 1h);
    CHECK(pruned);
    CHECK(*pruned == 5);
    CHECK(std::filesystem::exists(directory.path / "20261027T210000+0900.json"));
    CHECK(!std::filesystem::exists(directory.path / "20261027T100000Z_9999.json"));

    TemporaryDirectory daylight_directory;
    monitor::JsonSnapshotStore daylight_store(daylight_directory.path);
    timezone.set("EST5EDT,M3.2.0/2,M11.1.0/2");
    const auto daylight = std::chrono::sys_days(std::chrono::year{2026} / 11 / 1) + 5h + 30min;
    CHECK(daylight_store.save(make_snapshot(daylight)));
    CHECK(daylight_store.save(make_snapshot(daylight + 1h)));
    timezone.set("KST-9");
    const auto daylight_pruned = daylight_store.prune(daylight + 30min);
    CHECK(daylight_pruned);
    CHECK(*daylight_pruned == 1);
    CHECK(!std::filesystem::exists(daylight_directory.path / "20261101T013000-0400.json"));
    CHECK(std::filesystem::exists(daylight_directory.path / "20261101T013000-0500.json"));
}

void check_only_generated_regular_files_are_removed() {
    ScopedTimezone timezone("UTC0");
    TemporaryDirectory directory;
    TemporaryDirectory external;
    monitor::JsonSnapshotStore store(directory.path);
    CHECK(store.save(make_snapshot(base_time())));
    const auto original = directory.path / "20261027T100000+0000.json";
    const auto document = read_json(original);
    const std::vector<std::string> preserved_names{
        "settings.json", "20260230T100000+0000.json", "20261027T250000+0000.json",
        "20261027T100060+0000.json", "20261027T100000+0060.json", "20261027T100000+2400.json",
        "20261027T100000+0000_0.json", "20261027T100000+0000_01.json",
        "20261027T100000+0000_10000.json", "20261027T100000+0000-copy.json",
        "20261027T100000+0000.json.bak", ".resource-monitor.123.1.tmp", "20261027T100000+0000.done"};
    for (const auto& name : preserved_names) write_json(directory.path / name, document);
    write_text(directory.path / "20261027T100001+0000.json", "{\"purpose\":\"another application\"}");
    write_json(directory.path / "20261027T100002+0000.json", document);
    auto wrong_schema = document;
    wrong_schema["timestamp"] = "2026-10-27T10:00:03+00:00";
    wrong_schema["schema_version"] = 2;
    write_json(directory.path / "20261027T100003+0000.json", wrong_schema);
    write_text(directory.path / "20261027T100004+0000.json", "{\"schema_version\":1,");
    write_json(directory.path / "20261027T100005+0000.json", document);
    std::filesystem::resize_file(directory.path / "20261027T100005+0000.json", 17 * 1024 * 1024);

    auto outside_document = document;
    outside_document["timestamp"] = "2026-10-27T10:00:06+00:00";
    const auto outside_file = external.path / "20261027T100006+0000.json";
    write_json(outside_file, outside_document);
    std::filesystem::create_symlink(outside_file, directory.path / "20261027T100006+0000.json");
    std::filesystem::create_directory_symlink(external.path, directory.path / "linked-directory");
    std::filesystem::create_directory(directory.path / "nested");
    write_json(directory.path / "nested/20261027T100000+0000.json", document);

    auto hardlinked = document;
    hardlinked["timestamp"] = "2026-10-27T10:00:07+00:00";
    const auto linked_file = directory.path / "20261027T100007+0000.json";
    write_json(linked_file, hardlinked);
    std::filesystem::create_hard_link(linked_file, external.path / "hardlink.json");
    CHECK(::mkfifo((directory.path / "20261027T100008+0000.json").c_str(), 0600) == 0);
    std::filesystem::create_directory(directory.path / "20261027T100009+0000.json");
    write_json(directory.path / "20261027T100009+0000.json/20261027T100000+0000.json", document);
    const auto pruned = store.prune(base_time() + 1h);
    CHECK(pruned);
    CHECK(*pruned == 1);
    CHECK(!std::filesystem::exists(original));
    for (const auto& name : preserved_names) CHECK(std::filesystem::is_regular_file(directory.path / name));
    for (unsigned second = 1; second <= 5; ++second) {
        CHECK(std::filesystem::is_regular_file(directory.path / ("20261027T10000" + std::to_string(second) + "+0000.json")));
    }
    CHECK(std::filesystem::is_symlink(directory.path / "20261027T100006+0000.json"));
    CHECK(std::filesystem::is_symlink(directory.path / "linked-directory"));
    CHECK(read_json(outside_file) == outside_document);
    CHECK(read_json(directory.path / "nested/20261027T100000+0000.json") == document);
    CHECK(read_json(external.path / "hardlink.json") == hardlinked);
    CHECK(std::filesystem::hard_link_count(linked_file) == 2);
    CHECK(std::filesystem::is_fifo(directory.path / "20261027T100008+0000.json"));
    CHECK(std::filesystem::is_directory(directory.path / "20261027T100009+0000.json"));
    CHECK(std::filesystem::is_regular_file(directory.path / ".resource-monitor.lock"));
}

void check_repeated_scans_and_reload() {
    ScopedTimezone timezone("UTC0");
    TemporaryDirectory first;
    TemporaryDirectory second;
    monitor::JsonSnapshotStore store(first.path);
    CHECK(prune_count(store, base_time()) == 0);
    const auto descriptors = count_descriptors();
    for (unsigned index = 0; index < 16; ++index) {
        CHECK(store.save(make_snapshot(base_time() + std::chrono::seconds{index})));
        const auto pruned = store.prune(base_time() + 1h);
        CHECK(pruned);
        CHECK(*pruned == 1);
        CHECK(prune_count(store, base_time() + 1h) == 0);
        CHECK(count_descriptors() == descriptors);
    }
    CHECK(store.save(make_snapshot(base_time())));
    CHECK(store.configure_output(second.path.string()));
    CHECK(store.save(make_snapshot(base_time())));
    CHECK(prune_count(store, base_time() + 1h) == 1);
    CHECK(std::filesystem::exists(first.path / "20261027T100000+0000.json"));
    CHECK(!std::filesystem::exists(second.path / "20261027T100000+0000.json"));
    CHECK(!store.configure_output((first.path / "absent").string()));
    CHECK(store.save(make_snapshot(base_time() + 1s)));
    CHECK(prune_count(store, base_time() + 1h) == 1);
    CHECK(std::filesystem::exists(first.path / "20261027T100000+0000.json"));
    CHECK(store.configure_output(first.path.string()));
    CHECK(prune_count(store, base_time() + 1h) == 1);
    CHECK(count_descriptors() == descriptors);
}

void check_pinned_directory() {
    ScopedTimezone timezone("UTC0");
    TemporaryDirectory parent;
    const auto named = parent.path / "current";
    const auto pinned = parent.path / "pinned";
    std::filesystem::create_directory(named);
    monitor::JsonSnapshotStore store(named);
    CHECK(store.save(make_snapshot(base_time())));
    const auto document = read_json(named / "20261027T100000+0000.json");
    std::filesystem::rename(named, pinned);
    std::filesystem::create_directory(named);
    write_json(named / "20261027T100000+0000.json", document);
    const auto pruned = store.prune(base_time() + 1h);
    CHECK(pruned);
    CHECK(*pruned == 1);
    CHECK(!std::filesystem::exists(pinned / "20261027T100000+0000.json"));
    CHECK(read_json(named / "20261027T100000+0000.json") == document);
    CHECK(store.configure_output(named.string()));
    CHECK(prune_count(store, base_time() + 1h) == 1);
}

void check_deletion_failure_is_reported_and_recoverable() {
    ScopedTimezone timezone("UTC0");
    TemporaryDirectory directory;
    monitor::JsonSnapshotStore store(directory.path);
    CHECK(store.save(make_snapshot(base_time())));
    const auto original = directory.path / "20261027T100000+0000.json";
    CHECK(::chmod(original.c_str(), 0644) == 0);
    CHECK(::chmod(directory.path.c_str(), 0555) == 0);
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        if (::geteuid() == 0 && (::setgid(65534) != 0 || ::setuid(65534) != 0)) ::_exit(2);
        const auto result = store.prune(base_time() + 1h);
        ::_exit(!result && result.error().component == "snapshot_retention" ? 0 : 1);
    }
    int status{};
    pid_t waited{};
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    CHECK(::chmod(directory.path.c_str(), 0700) == 0);
    CHECK(waited == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(std::filesystem::exists(original));
    CHECK(store.save(make_snapshot(base_time() + 2h)));
    CHECK(prune_count(store, base_time() + 1h) == 1);
    CHECK(std::filesystem::is_regular_file(directory.path / "20261027T120000+0000.json"));
}

}

int main() {
    return test::run([] {
        check_retention_boundary();
        check_configurable_retention_policy();
        check_offsets_legacy_and_collisions();
        check_only_generated_regular_files_are_removed();
        check_repeated_scans_and_reload();
        check_pinned_directory();
        check_deletion_failure_is_reported_and_recoverable();
    });
}
