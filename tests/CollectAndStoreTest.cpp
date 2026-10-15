// tests/CollectAndStoreTest.cpp
#include "TestSupport.hpp"
#include "application/CollectAndStore.hpp"

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
    CHECK(store.attempts.empty());
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
    CHECK(store.attempts.size() == 4);

    reader.next = sample(6);
    reader.before_read = [&] { config.next = ThresholdPolicy{0.0, 0.0, 0.0}; };
    auto in_flight = use_case.run();
    CHECK(in_flight);
    CHECK(in_flight->status == MetricStatus::ok);
    CHECK(in_flight->applied_thresholds.cpu_usage_percent == 80.0);
    CHECK(store.attempts.size() == 4);
    reader.before_read = {};
    reader.next = sample(7);
    auto next_cycle = use_case.run();
    CHECK(next_cycle);
    CHECK(next_cycle->cpu.status == MetricStatus::warning);
    CHECK(next_cycle->memory.status == MetricStatus::warning);
    CHECK(next_cycle->filesystems.front().status == MetricStatus::warning);
    CHECK(store.attempts.size() == 5);
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
    CHECK(store.attempts.size() == 1);

    reader.next = sample(3);
    auto recovered = use_case.run();
    CHECK(recovered);
    CHECK(recovered->cpu.status == MetricStatus::ok);
    CHECK(test::near(*recovered->cpu.window_seconds, 60.0));
    CHECK(store.attempts.size() == 1);

    reader.next = sample(4);
    reader.next.filesystems.clear();
    auto empty = use_case.run();
    CHECK(empty);
    CHECK(empty->status == MetricStatus::unknown);
    CHECK(empty->collection_status == CollectionStatus::partial);
    CHECK(has_error(*empty, "no_filesystems"));
    CHECK(store.attempts.size() == 1);

    reader.next = sample(5);
    reader.next.cpu.value.reset();
    reader.next.memory.value.reset();
    reader.next.filesystems.clear();
    auto unavailable = use_case.run();
    CHECK(unavailable);
    CHECK(unavailable->collection_status == CollectionStatus::error);
    CHECK(unavailable->status == MetricStatus::unknown);
    CHECK(store.attempts.size() == 1);
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
    CHECK(store.attempts.size() == 1);
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
    CHECK(store.attempts.empty());
    reader.next = sample(12);
    reader.next.cpu.value = CpuCounters{{51, 0, 0, 51, 0, 0, 0, 0}};
    auto recovered = use_case.run();
    CHECK(recovered);
    CHECK(recovered->cpu.usage_percent == 50.0);
    CHECK(recovered->cpu.window_seconds == 60.0);
    CHECK(store.attempts.empty());
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

void errors_without_warning_are_not_saved_test() {
    Reader reader;
    Store store;
    Config config;
    CollectAndStore use_case(reader, store, config);
    CHECK(use_case.initialize());
    store.failure = Error{"store", "/Monitor", "disk_full", "No free space"};
    config.next = std::unexpected(Error{"thresholds", "config", "invalid_json", "Incomplete JSON"});
    reader.next = sample(1);
    auto invalid_config = use_case.run();
    CHECK(invalid_config);
    CHECK(invalid_config->status == MetricStatus::ok);
    CHECK(invalid_config->config_status == ConfigStatus::using_last_valid);
    CHECK(has_error(*invalid_config, "invalid_json"));
    CHECK(store.attempts.empty());

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
    CHECK(store.attempts.empty());
}

void skipped_snapshot_keeps_cpu_baseline_test() {
    Reader reader;
    Store store;
    Config config;
    CollectAndStore use_case(reader, store, config);
    CHECK(use_case.initialize());
    reader.next = sample(1);
    reader.next.cpu.value = CpuCounters{{20, 0, 0, 80, 0, 0, 0, 0}};
    auto skipped = use_case.run();
    CHECK(skipped);
    CHECK(skipped->status == MetricStatus::ok);
    CHECK(skipped->cpu.usage_percent == 20.0);
    CHECK(store.attempts.empty());

    reader.next = sample(2);
    reader.next.cpu.value = CpuCounters{{100, 0, 0, 100, 0, 0, 0, 0}};
    auto warning = use_case.run();
    CHECK(warning);
    CHECK(warning->cpu.usage_percent == 80.0);
    CHECK(warning->cpu.window_seconds == 60.0);
    CHECK(warning->status == MetricStatus::warning);
    CHECK(store.attempts.size() == 1);

    reader.next = sample(3);
    reader.next.cpu.value = CpuCounters{{100, 0, 0, 200, 0, 0, 0, 0}};
    auto recovered = use_case.run();
    CHECK(recovered);
    CHECK(recovered->status == MetricStatus::ok);
    CHECK(recovered->cpu.usage_percent == 0.0);
    CHECK(recovered->cpu.window_seconds == 60.0);
    CHECK(store.attempts.size() == 1);
}

}

int main() {
    return test::run([] {
        initialization_tests();
        reload_tests();
        partial_failure_tests();
        write_failure_baseline_test();
        storage_warning_and_unknown_test();
        counter_regression_recovery_test();
        individual_resource_boundary_tests();
        errors_without_warning_are_not_saved_test();
        skipped_snapshot_keeps_cpu_baseline_test();
    });
}
