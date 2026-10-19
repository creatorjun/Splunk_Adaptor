// tests/CollectAndStoreTest.cpp
#include "TestSupport.hpp"
#include "application/CollectAndStore.hpp"
#include "application/SettingsController.hpp"

#include <algorithm>
#include <functional>
#include <limits>

namespace {

using namespace monitor;

ResourceSample sample(std::uint64_t minute) {
    ResourceSample result;
    result.timestamp = std::chrono::system_clock::time_point{std::chrono::seconds{100000 + minute * 60}};
    result.cpu_observed_at = std::chrono::steady_clock::time_point{std::chrono::seconds{minute * 60}};
    result.hostname = "test-host";
    result.cpu.value = CpuCounters{{minute * 50, 0, 0, minute * 50, 0, 0, 0, 0}};
    result.memory.value = MemoryCounters{1000, 400};
    FilesystemSample filesystem;
    filesystem.device = "/dev/mapper/data";
    filesystem.device_id = "253:1";
    filesystem.filesystem_type = "xfs";
    filesystem.mount_points = {"/data"};
    filesystem.counters.value = FilesystemCounters{100, 50, 50, 4096};
    result.filesystems.push_back(filesystem);
    return result;
}

struct Reader final : IResourceReader {
    ResourceSample next = sample(0);
    std::function<void()> before_read;
    std::size_t reads{};

    ResourceSample read() override {
        ++reads;
        if (before_read) {
            before_read();
        }
        return next;
    }
};

struct Store final : ISnapshotStore {
    std::vector<ResourceSnapshot> attempts;
    std::optional<Error> failure;

    std::expected<void, Error> save(const ResourceSnapshot& snapshot) override {
        attempts.push_back(snapshot);
        if (failure) {
            return std::unexpected(*failure);
        }
        return {};
    }
};

struct Config final : IThresholdConfigReader {
    std::expected<ThresholdPolicy, Error> next = ThresholdPolicy{80.0, 80.0, 80.0};
    std::size_t reads{};

    std::expected<ThresholdPolicy, Error> read() override {
        ++reads;
        return next;
    }
};

struct SettingsReader final : ISettingsReader {
    std::expected<MonitorSettings, Error> next =
        MonitorSettings{ThresholdPolicy{80.0, 80.0, 80.0}, std::chrono::seconds{60}, "/Monitor"};

    std::expected<MonitorSettings, Error> read_settings() override {
        return next;
    }
};

struct OutputDirectory final : IOutputDirectory {
    std::string active;

    std::expected<void, Error> configure_output(const std::string& directory) override {
        active = directory;
        return {};
    }
};

struct CountMonitor final : ICollectCountMonitor {
    CollectCountAnalysis next;
    std::vector<std::chrono::system_clock::time_point> observed;
    std::vector<std::chrono::system_clock::time_point> settings_updates;

    CollectCountAnalysis collect(std::chrono::system_clock::time_point timestamp) override {
        observed.push_back(timestamp);
        return next;
    }

    void settings_changed(std::chrono::system_clock::time_point timestamp) override {
        settings_updates.push_back(timestamp);
    }
};

struct ProcessMonitor final : IJavaProcessMonitor {
    JavaProcessSnapshot next;
    unsigned reads{};

    JavaProcessSnapshot collect() override {
        ++reads;
        return next;
    }
};

bool has_error(const ResourceSnapshot& snapshot, const std::string& code) {
    return std::ranges::any_of(snapshot.errors, [&](const Error& error) {
        return error.code == code;
    });
}

void initialization_tests() {
    Reader reader;
    Store store;
    Config config;
    CollectAndStore use_case(reader, store, config);
    CHECK(!use_case.run());
    CHECK(reader.reads == 0);
    CHECK(config.reads == 0);
    config.next = std::unexpected(Error{"thresholds", "config", "invalid_json", "Invalid JSON"});
    CHECK(!use_case.initialize());
    CHECK(reader.reads == 0);
    config.next = ThresholdPolicy{80.0, -1.0, 80.0};
    CHECK(!use_case.initialize());
    CHECK(reader.reads == 0);
    config.next = ThresholdPolicy{80.0, 80.0, 80.0};
    CHECK(use_case.initialize());
    CHECK(reader.reads == 1);
    CHECK(store.attempts.empty());
    reader.next = sample(1);
    auto result = use_case.run(2);
    CHECK(result);
    CHECK(result->cpu.status == MetricStatus::ok);
    CHECK(test::near(*result->cpu.usage_percent, 50.0));
    CHECK(test::near(*result->cpu.window_seconds, 60.0));
    CHECK(result->missed_intervals == 2);
    CHECK(result->hostname == "test-host");
    CHECK(result->collection_status == CollectionStatus::ok);
    CHECK(result->thresholds_applied_at == sample(0).timestamp);
    CHECK(store.attempts.size() == 1);
    CHECK(store.attempts.front().status == MetricStatus::ok);
    CHECK(store.attempts.front().timestamp == sample(1).timestamp);
    CHECK(result->collect_count.status == CollectCountStatus::disabled);
}

void count_analysis_tests() {
    Reader reader;
    Store store;
    Config config;
    CountMonitor monitor;
    monitor.next.status = CollectCountStatus::pending;
    CollectAndStore use_case(reader, store, config, &monitor);
    CHECK(!use_case.run());
    CHECK(monitor.observed.empty());
    use_case.settings_changed(sample(0).timestamp);
    CHECK(monitor.settings_updates.empty());
    CHECK(use_case.initialize());
    CHECK(monitor.observed == std::vector{sample(0).timestamp});
    CHECK(store.attempts.empty());
    const auto initial_reads = reader.reads;
    use_case.settings_changed(sample(1).timestamp);
    CHECK(monitor.settings_updates == std::vector{sample(1).timestamp});
    CHECK(reader.reads == initial_reads && store.attempts.empty());

    monitor.next = *analyze_collect_counts({{"10.1.0.1", 110, 100, true, true}}, 10.0);
    monitor.next.window = {20261018, 20261017};
    monitor.next.queried_at = sample(0).timestamp;
    reader.next = sample(1);
    auto result = use_case.run();
    CHECK(result);
    CHECK(result->status == MetricStatus::warning);
    CHECK(result->collection_status == CollectionStatus::ok);
    CHECK(result->cpu.status == MetricStatus::ok);
    CHECK(result->memory.status == MetricStatus::ok);
    CHECK(result->filesystems.front().status == MetricStatus::ok);
    CHECK(result->collect_count.status == CollectCountStatus::warning);
    CHECK(result->collect_count.window == monitor.next.window);
    CHECK(result->collect_count.queried_at == monitor.next.queried_at);
    CHECK(result->collect_count.alerts.size() == 1);
    CHECK(result->collect_count.alerts.front().change_percent == 10.0);
    CHECK(store.attempts.back().collect_count.alerts.front().eqp_ip == "10.1.0.1");

    monitor.next.status = CollectCountStatus::error;
    monitor.next.alerts.clear();
    monitor.next.error = Error{"collect_count", "CloudESM.SUM_COLLECT_INFO_1DAY",
        "query_failed", "Database unavailable"};
    reader.next = sample(2);
    result = use_case.run();
    CHECK(result);
    CHECK(result->status == MetricStatus::unknown);
    CHECK(result->collection_status == CollectionStatus::ok);
    CHECK(result->cpu.usage_percent == 50.0);
    CHECK(result->memory.usage_percent == 60.0);
    CHECK(result->collect_count.error->code == "query_failed");
    CHECK(has_error(*result, "query_failed"));
    CHECK(store.attempts.back().status == MetricStatus::unknown);
    CHECK(has_error(store.attempts.back(), "query_failed"));

    reader.next = sample(3);
    reader.next.memory.value = MemoryCounters{1000, 100};
    result = use_case.run();
    CHECK(result);
    CHECK(result->status == MetricStatus::warning);
    CHECK(result->collection_status == CollectionStatus::ok);
    CHECK(result->collect_count.status == CollectCountStatus::error);

    monitor.next = {};
    std::uint64_t minute = 4;
    for (const auto state : {CollectCountStatus::pending, CollectCountStatus::no_data,
        CollectCountStatus::ok, CollectCountStatus::disabled}) {
        monitor.next.status = state;
        reader.next = sample(minute++);
        result = use_case.run();
        CHECK(result);
        CHECK(result->collection_status == CollectionStatus::ok);
        CHECK(result->status == (state == CollectCountStatus::ok || state == CollectCountStatus::disabled
            ? MetricStatus::ok : MetricStatus::unknown));
        CHECK(result->collect_count.status == state);
        CHECK(result->errors.empty());
    }
    CHECK(store.attempts.size() == 7);
    CHECK(monitor.observed.size() == 8);
    CHECK(monitor.observed.back() == sample(7).timestamp);
}

void process_monitor_tests() {
    Reader reader;
    Store store;
    Config config;
    CountMonitor counts;
    ProcessMonitor processes;
    processes.next.status = MetricStatus::warning;
    processes.next.apps = {{"SearchEngine", true}, {"Reducer", false}};
    CollectAndStore use_case(reader, store, config, &counts, &processes);
    CHECK(!use_case.run());
    CHECK(processes.reads == 0);
    CHECK(use_case.initialize());
    CHECK(processes.reads == 0);
    reader.next = sample(1);
    auto result = use_case.run();
    CHECK(result);
    CHECK(processes.reads == 1);
    CHECK(result->status == MetricStatus::warning);
    CHECK(result->collection_status == CollectionStatus::ok);
    CHECK(result->java_processes.status == MetricStatus::warning);
    CHECK(result->java_processes.apps.at("SearchEngine") == true);
    CHECK(result->java_processes.apps.at("Reducer") == false);
    CHECK(store.attempts.back().java_processes.apps == processes.next.apps);

    processes.next.status = MetricStatus::unknown;
    processes.next.apps["Reducer"].reset();
    processes.next.errors = {{"java_processes", "/proc", "read_failed", "Cannot enumerate processes"}};
    reader.next = sample(2);
    result = use_case.run();
    CHECK(result);
    CHECK(result->status == MetricStatus::unknown);
    CHECK(result->collection_status == CollectionStatus::ok);
    CHECK(has_error(*result, "read_failed"));
    CHECK(has_error(store.attempts.back(), "read_failed"));
    CHECK(!result->java_processes.apps.at("Reducer").has_value());
    CHECK(result->cpu.usage_percent == 50.0);

    reader.next = sample(3);
    reader.next.memory.value = MemoryCounters{1000, 100};
    result = use_case.run();
    CHECK(result);
    CHECK(result->status == MetricStatus::warning);
    CHECK(result->java_processes.status == MetricStatus::unknown);

    reader.next = sample(4);
    counts.next.status = CollectCountStatus::warning;
    result = use_case.run();
    CHECK(result);
    CHECK(result->status == MetricStatus::warning);

    reader.next = sample(5);
    counts.next.status = CollectCountStatus::error;
    processes.next.status = MetricStatus::warning;
    processes.next.apps["Reducer"] = false;
    processes.next.errors.clear();
    result = use_case.run();
    CHECK(result);
    CHECK(result->status == MetricStatus::warning);

    reader.next = sample(6);
    processes.next.status = MetricStatus::ok;
    processes.next.apps["Reducer"] = true;
    result = use_case.run();
    CHECK(result);
    CHECK(result->status == MetricStatus::unknown);

    reader.next = sample(7);
    counts.next.status = CollectCountStatus::disabled;
    result = use_case.run();
    CHECK(result);
    CHECK(result->status == MetricStatus::ok);
    CHECK(result->collection_status == CollectionStatus::ok);
    CHECK(result->errors.empty());
    CHECK(processes.reads == 7);
    CHECK(store.attempts.size() == 7);
}

void reload_tests() {
    Reader reader;
    Store store;
    Config config;
    CollectAndStore use_case(reader, store, config);
    CHECK(use_case.initialize());
    reader.next = sample(1);
    config.next = ThresholdPolicy{50.0, 80.0, 80.0};
    auto warning = use_case.run();
    CHECK(warning);
    CHECK(warning->status == MetricStatus::warning);
    CHECK(warning->cpu.status == MetricStatus::warning);
    CHECK(warning->thresholds_applied_at == sample(1).timestamp);
    CHECK(store.attempts.size() == 1);

    reader.next = sample(2);
    auto unchanged = use_case.run();
    CHECK(unchanged);
    CHECK(unchanged->thresholds_applied_at == sample(1).timestamp);
    CHECK(store.attempts.size() == 2);
    CHECK(store.attempts.back().timestamp == sample(2).timestamp);

    reader.next = sample(3);
    config.next = std::unexpected(Error{"thresholds", "config", "invalid_json", "Incomplete editor write"});
    auto invalid_json = use_case.run();
    CHECK(invalid_json);
    CHECK(invalid_json->config_status == ConfigStatus::using_last_valid);
    CHECK(invalid_json->collection_status == CollectionStatus::ok);
    CHECK(invalid_json->cpu.status == MetricStatus::warning);
    CHECK(invalid_json->thresholds_applied_at == sample(1).timestamp);
    CHECK(has_error(*invalid_json, "invalid_json"));
    CHECK(store.attempts.size() == 3);

    reader.next = sample(4);
    config.next = ThresholdPolicy{99.0, std::numeric_limits<double>::quiet_NaN(), 99.0};
    auto invalid_policy = use_case.run();
    CHECK(invalid_policy);
    CHECK(invalid_policy->config_status == ConfigStatus::using_last_valid);
    CHECK(invalid_policy->applied_thresholds.cpu_usage_percent == 50.0);
    CHECK(invalid_policy->applied_thresholds.storage_usage_percent == 80.0);
    CHECK(invalid_policy->collection_status == CollectionStatus::ok);
    CHECK(has_error(*invalid_policy, "invalid_threshold"));
    CHECK(store.attempts.size() == 4);

    reader.next = sample(5);
    config.next = ThresholdPolicy{80.0, 80.0, 80.0};
    auto recovered = use_case.run();
    CHECK(recovered);
    CHECK(recovered->status == MetricStatus::ok);
    CHECK(recovered->config_status == ConfigStatus::ok);
    CHECK(recovered->thresholds_applied_at == sample(5).timestamp);
    CHECK(recovered->errors.empty());
    CHECK(store.attempts.size() == 5);
    CHECK(store.attempts.back().status == MetricStatus::ok);

    reader.next = sample(6);
    reader.before_read = [&] { config.next = ThresholdPolicy{0.0, 0.0, 0.0}; };
    auto in_flight = use_case.run();
    CHECK(in_flight);
    CHECK(in_flight->status == MetricStatus::ok);
    CHECK(in_flight->applied_thresholds.cpu_usage_percent == 80.0);
    CHECK(store.attempts.size() == 6);
    reader.before_read = {};
    reader.next = sample(7);
    auto next_cycle = use_case.run();
    CHECK(next_cycle);
    CHECK(next_cycle->cpu.status == MetricStatus::warning);
    CHECK(next_cycle->memory.status == MetricStatus::warning);
    CHECK(next_cycle->filesystems.front().status == MetricStatus::warning);
    CHECK(store.attempts.size() == 7);
    for (std::size_t index = 0; index < store.attempts.size(); ++index) {
        CHECK(store.attempts[index].timestamp == sample(index + 1).timestamp);
    }
}

void partial_failure_tests() {
    Reader reader;
    Store store;
    Config config;
    CollectAndStore use_case(reader, store, config);
    CHECK(use_case.initialize());
    reader.next = sample(1);
    reader.next.cpu.value.reset();
    reader.next.cpu.error = Error{"cpu", "proc", "read_failed", "CPU input unavailable"};
    reader.next.memory.value = MemoryCounters{1000, 100};
    reader.next.filesystems.front().counters.value.reset();
    reader.next.filesystems.front().counters.error = Error{"storage", "/data", "unmounted", "Mount vanished"};
    reader.next.filesystem_enumeration_error = Error{"storage", "mounts", "enumeration_failed", "Partial enumeration"};
    auto failed = use_case.run();
    CHECK(failed);
    CHECK(failed->status == MetricStatus::warning);
    CHECK(failed->collection_status == CollectionStatus::partial);
    CHECK(failed->cpu.status == MetricStatus::unknown);
    CHECK(!failed->cpu.usage_percent);
    CHECK(failed->filesystems.front().mount_points == std::vector<std::string>{"/data"});
    CHECK(!failed->filesystems.front().total_bytes);
    CHECK(has_error(*failed, "read_failed"));
    CHECK(has_error(*failed, "unmounted"));
    CHECK(has_error(*failed, "enumeration_failed"));
    CHECK(store.attempts.size() == 1);
    CHECK(store.attempts.back().collection_status == CollectionStatus::partial);

    reader.next = sample(2);
    auto baseline = use_case.run();
    CHECK(baseline);
    CHECK(baseline->cpu.status == MetricStatus::unknown);
    CHECK(baseline->status == MetricStatus::unknown);
    CHECK(has_error(*baseline, "insufficient_baseline"));
    CHECK(store.attempts.size() == 2);
    CHECK(store.attempts.back().status == MetricStatus::unknown);
    CHECK(has_error(store.attempts.back(), "insufficient_baseline"));

    reader.next = sample(3);
    auto recovered = use_case.run();
    CHECK(recovered);
    CHECK(recovered->cpu.status == MetricStatus::ok);
    CHECK(test::near(*recovered->cpu.window_seconds, 60.0));
    CHECK(store.attempts.size() == 3);
    CHECK(store.attempts.back().status == MetricStatus::ok);

    reader.next = sample(4);
    reader.next.filesystems.clear();
    auto empty = use_case.run();
    CHECK(empty);
    CHECK(empty->status == MetricStatus::unknown);
    CHECK(empty->collection_status == CollectionStatus::partial);
    CHECK(has_error(*empty, "no_filesystems"));
    CHECK(store.attempts.size() == 4);
    CHECK(has_error(store.attempts.back(), "no_filesystems"));

    reader.next = sample(5);
    reader.next.cpu.value.reset();
    reader.next.memory.value.reset();
    reader.next.filesystems.clear();
    auto unavailable = use_case.run();
    CHECK(unavailable);
    CHECK(unavailable->collection_status == CollectionStatus::error);
    CHECK(unavailable->status == MetricStatus::unknown);
    CHECK(store.attempts.size() == 5);
    CHECK(store.attempts.back().status == MetricStatus::unknown);
    CHECK(store.attempts.back().collection_status == CollectionStatus::error);
    CHECK(has_error(store.attempts.back(), "missing_reading"));
}

void write_failure_baseline_test() {
    Reader reader;
    Store store;
    Config config;
    CollectAndStore use_case(reader, store, config);
    CHECK(use_case.initialize());
    reader.next = sample(1);
    reader.next.cpu.value = CpuCounters{{100, 0, 0, 0, 0, 0, 0, 0}};
    config.next = std::unexpected(Error{"thresholds", "config", "invalid_json", "Invalid JSON during disk failure"});
    store.failure = Error{"store", "/Monitor", "disk_full", "No free space"};
    auto failed = use_case.run();
    CHECK(!failed);
    CHECK(failed.error().code == "disk_full");
    CHECK(failed.error().message.find("component=thresholds") != std::string::npos);
    CHECK(failed.error().message.find("target=config") != std::string::npos);
    CHECK(failed.error().message.find("code=invalid_json") != std::string::npos);
    CHECK(failed.error().message.find("Invalid JSON during disk failure") != std::string::npos);
    CHECK(store.attempts.size() == 1);
    CHECK(store.attempts.front().cpu.usage_percent == 100.0);

    reader.next = sample(2);
    reader.next.cpu.value = CpuCounters{{100, 0, 0, 100, 0, 0, 0, 0}};
    config.next = ThresholdPolicy{80.0, 80.0, 80.0};
    store.failure.reset();
    auto recovered = use_case.run();
    CHECK(recovered);
    CHECK(recovered->cpu.usage_percent == 0.0);
    CHECK(recovered->cpu.window_seconds == 60.0);
    CHECK(store.attempts.size() == 2);
    CHECK(store.attempts.back().status == MetricStatus::ok);
}

void storage_warning_and_unknown_test() {
    Reader reader;
    Store store;
    Config config;
    CollectAndStore use_case(reader, store, config);
    CHECK(use_case.initialize());
    reader.next = sample(1);
    reader.next.filesystems.front().counters.value = FilesystemCounters{100, 10, 10, 4096};
    auto reserved = reader.next.filesystems.front();
    reserved.device = "/dev/mapper/reserved";
    reserved.device_id = "253:2";
    reserved.mount_points = {"/reserved"};
    reserved.counters.value = FilesystemCounters{100, 100, 0, 4096};
    reader.next.filesystems.push_back(reserved);
    auto result = use_case.run();
    CHECK(result);
    CHECK(result->cpu.status == MetricStatus::ok);
    CHECK(result->memory.status == MetricStatus::ok);
    CHECK(result->filesystems[0].status == MetricStatus::warning);
    CHECK(result->filesystems[1].status == MetricStatus::unknown);
    CHECK(result->filesystems[1].total_bytes == 409600);
    CHECK(result->filesystems[1].used_bytes == 0);
    CHECK(result->filesystems[1].available_bytes == 0);
    CHECK(!result->filesystems[1].usage_percent);
    CHECK(result->status == MetricStatus::warning);
    CHECK(result->collection_status == CollectionStatus::partial);
    CHECK(has_error(*result, "zero_usable_capacity"));
    CHECK(store.attempts.size() == 1);
    CHECK(store.attempts.front().filesystems.size() == 2);
}

void counter_regression_recovery_test() {
    Reader reader;
    Store store;
    Config config;
    reader.next = sample(10);
    CollectAndStore use_case(reader, store, config);
    CHECK(use_case.initialize());
    reader.next = sample(11);
    reader.next.cpu.value = CpuCounters{{1, 0, 0, 1, 0, 0, 0, 0}};
    auto regressed = use_case.run();
    CHECK(regressed);
    CHECK(regressed->cpu.status == MetricStatus::unknown);
    CHECK(has_error(*regressed, "counter_regression"));
    CHECK(store.attempts.size() == 1);
    CHECK(has_error(store.attempts.back(), "counter_regression"));
    reader.next = sample(12);
    reader.next.cpu.value = CpuCounters{{51, 0, 0, 51, 0, 0, 0, 0}};
    auto recovered = use_case.run();
    CHECK(recovered);
    CHECK(recovered->cpu.usage_percent == 50.0);
    CHECK(recovered->cpu.window_seconds == 60.0);
    CHECK(store.attempts.size() == 2);
    CHECK(store.attempts.back().status == MetricStatus::ok);
}

void individual_resource_boundary_tests() {
    for (unsigned resource = 0; resource < 3; ++resource) {
        Reader reader;
        Store store;
        Config config;
        CollectAndStore use_case(reader, store, config);
        CHECK(use_case.initialize());
        reader.next = sample(1);
        if (resource == 0) {
            reader.next.cpu.value = CpuCounters{{80, 0, 0, 20, 0, 0, 0, 0}};
        } else if (resource == 1) {
            reader.next.memory.value = MemoryCounters{1000, 200};
        } else {
            auto disk = reader.next.filesystems.front();
            disk.device = "/dev/mapper/second";
            disk.device_id = "253:2";
            disk.mount_points = {"/second"};
            disk.counters.value = FilesystemCounters{100, 20, 20, 4096};
            reader.next.filesystems.push_back(disk);
        }
        const auto result = use_case.run();
        CHECK(result);
        CHECK(result->status == MetricStatus::warning);
        CHECK(result->collection_status == CollectionStatus::ok);
        CHECK(result->cpu.status == (resource == 0 ? MetricStatus::warning : MetricStatus::ok));
        CHECK(result->memory.status == (resource == 1 ? MetricStatus::warning : MetricStatus::ok));
        CHECK(result->filesystems.front().status == MetricStatus::ok);
        if (resource == 2) {
            CHECK(result->filesystems.back().status == MetricStatus::warning);
            CHECK(result->filesystems.back().usage_percent == 80.0);
        }
        CHECK(store.attempts.size() == 1);
        CHECK(store.attempts.front().timestamp == result->timestamp);
    }
}

void errors_without_warning_are_saved_test() {
    Reader reader;
    Store store;
    Config config;
    CollectAndStore use_case(reader, store, config);
    CHECK(use_case.initialize());
    config.next = std::unexpected(Error{"thresholds", "config", "invalid_json", "Incomplete JSON"});
    reader.next = sample(1);
    auto invalid_config = use_case.run();
    CHECK(invalid_config);
    CHECK(invalid_config->status == MetricStatus::ok);
    CHECK(invalid_config->config_status == ConfigStatus::using_last_valid);
    CHECK(has_error(*invalid_config, "invalid_json"));
    CHECK(store.attempts.size() == 1);
    CHECK(store.attempts.back().status == MetricStatus::ok);
    CHECK(has_error(store.attempts.back(), "invalid_json"));

    reader.next = sample(2);
    reader.next.memory.value.reset();
    reader.next.memory.error = Error{"memory", "proc", "read_failed", "Memory input unavailable"};
    auto unknown = use_case.run();
    CHECK(unknown);
    CHECK(unknown->status == MetricStatus::unknown);
    CHECK(unknown->collection_status == CollectionStatus::partial);
    CHECK(unknown->config_status == ConfigStatus::using_last_valid);
    CHECK(has_error(*unknown, "invalid_json"));
    CHECK(has_error(*unknown, "read_failed"));
    CHECK(store.attempts.size() == 2);
    CHECK(store.attempts.back().status == MetricStatus::unknown);
    CHECK(has_error(store.attempts.back(), "read_failed"));
}

void normal_write_failure_test() {
    Reader reader;
    Store store;
    Config config;
    CollectAndStore use_case(reader, store, config);
    CHECK(use_case.initialize());
    reader.next = sample(1);
    store.failure = Error{"store", "/Monitor", "disk_full", "No free space"};
    const auto failed = use_case.run();
    CHECK(!failed);
    CHECK(failed.error().code == "disk_full");
    CHECK(store.attempts.size() == 1);
    CHECK(store.attempts.front().status == MetricStatus::ok);
    CHECK(store.attempts.front().collection_status == CollectionStatus::ok);

    reader.next = sample(2);
    reader.next.cpu.value = CpuCounters{{50, 0, 0, 150, 0, 0, 0, 0}};
    store.failure.reset();
    const auto recovered = use_case.run();
    CHECK(recovered);
    CHECK(recovered->status == MetricStatus::ok);
    CHECK(recovered->cpu.usage_percent == 0.0);
    CHECK(recovered->cpu.window_seconds == 60.0);
    CHECK(store.attempts.size() == 2);
    CHECK(store.attempts.back().timestamp == sample(2).timestamp);
}

void every_snapshot_keeps_cpu_baseline_test() {
    Reader reader;
    Store store;
    Config config;
    CollectAndStore use_case(reader, store, config);
    CHECK(use_case.initialize());
    reader.next = sample(1);
    reader.next.cpu.value = CpuCounters{{20, 0, 0, 80, 0, 0, 0, 0}};
    auto normal = use_case.run();
    CHECK(normal);
    CHECK(normal->status == MetricStatus::ok);
    CHECK(normal->cpu.usage_percent == 20.0);
    CHECK(store.attempts.size() == 1);
    CHECK(store.attempts.back().status == MetricStatus::ok);

    reader.next = sample(2);
    reader.next.cpu.value = CpuCounters{{100, 0, 0, 100, 0, 0, 0, 0}};
    auto warning = use_case.run();
    CHECK(warning);
    CHECK(warning->cpu.usage_percent == 80.0);
    CHECK(warning->cpu.window_seconds == 60.0);
    CHECK(warning->status == MetricStatus::warning);
    CHECK(store.attempts.size() == 2);
    CHECK(store.attempts.back().status == MetricStatus::warning);

    reader.next = sample(3);
    reader.next.cpu.value = CpuCounters{{100, 0, 0, 200, 0, 0, 0, 0}};
    auto recovered = use_case.run();
    CHECK(recovered);
    CHECK(recovered->status == MetricStatus::ok);
    CHECK(recovered->cpu.usage_percent == 0.0);
    CHECK(recovered->cpu.window_seconds == 60.0);
    CHECK(store.attempts.size() == 3);
    CHECK(store.attempts.back().status == MetricStatus::ok);
}

void accepted_settings_survive_later_failure_test() {
    for (const bool restore_original : {false, true}) {
        Reader reader;
        Store store;
        SettingsReader settings_reader;
        OutputDirectory output;
        SettingsController controller(settings_reader, output);
        CHECK(controller.initialize());
        CollectAndStore use_case(reader, store, controller);
        CHECK(use_case.initialize());
        MonitorSettings accepted{ThresholdPolicy{0.0, 0.0, 0.0},
            std::chrono::seconds{5}, "/updated"};
        settings_reader.next = accepted;
        CHECK(controller.reload());
        if (restore_original) {
            accepted.thresholds = ThresholdPolicy{80.0, 80.0, 80.0};
            settings_reader.next = accepted;
            CHECK(controller.reload());
        }
        settings_reader.next = std::unexpected(Error{
            "settings_config", "settings.json", "invalid_json", "Truncated JSON"});
        CHECK(!controller.reload());
        CHECK(controller.current() == accepted);
        CHECK(output.active == accepted.output_directory);
        reader.next = sample(1);
        const auto snapshot = use_case.run();
        CHECK(snapshot);
        CHECK(snapshot->applied_thresholds == accepted.thresholds);
        CHECK(snapshot->config_status == ConfigStatus::using_last_valid);
        CHECK(has_error(*snapshot, "invalid_json"));
        CHECK(snapshot->errors.size() == 1);
        CHECK(snapshot->collection_status == CollectionStatus::ok);
        CHECK(snapshot->status == (restore_original ? MetricStatus::ok : MetricStatus::warning));
        const auto applied_at = restore_original ? sample(0).timestamp : sample(1).timestamp;
        CHECK(snapshot->thresholds_applied_at == applied_at);

        settings_reader.next = accepted;
        const auto recovered = controller.reload();
        CHECK(recovered);
        CHECK(!*recovered);
        reader.next = sample(2);
        const auto next_snapshot = use_case.run();
        CHECK(next_snapshot);
        CHECK(next_snapshot->applied_thresholds == accepted.thresholds);
        CHECK(next_snapshot->config_status == ConfigStatus::ok);
        CHECK(next_snapshot->errors.empty());
        CHECK(next_snapshot->thresholds_applied_at == applied_at);
    }
}

}

int main() {
    return test::run([] {
        initialization_tests();
        count_analysis_tests();
        process_monitor_tests();
        reload_tests();
        partial_failure_tests();
        write_failure_baseline_test();
        storage_warning_and_unknown_test();
        counter_regression_recovery_test();
        individual_resource_boundary_tests();
        errors_without_warning_are_saved_test();
        normal_write_failure_test();
        every_snapshot_keeps_cpu_baseline_test();
        accepted_settings_survive_later_failure_test();
    });
}
