// tests/MetricCalculatorsTest.cpp
#include "TestSupport.hpp"
#include "domain/MetricCalculators.hpp"

#include <limits>

namespace {

using namespace monitor;

FilesystemSample filesystem(FilesystemCounters counters) {
    FilesystemSample sample;
    sample.device = "/dev/mapper/data";
    sample.device_id = "253:1";
    sample.filesystem_type = "xfs";
    sample.mount_points = {"/data", "/data-alias"};
    sample.counters.value = counters;
    return sample;
}

void cpu_tests() {
    const CpuCounters zero{};
    const CpuCounters current{{20, 5, 10, 40, 10, 5, 5, 5}};
    const auto result = calculate_cpu(zero, current, 60.0, 45.0);
    CHECK(result);
    CHECK(test::near(*result->usage_percent, 45.0));
    CHECK(test::near(*result->iowait_percent, 10.0));
    CHECK(test::near(*result->steal_percent, 5.0));
    CHECK(test::near(*result->window_seconds, 60.0));
    CHECK(result->status == MetricStatus::warning);
    CHECK(calculate_cpu(zero, current, 60.0, 45.1)->status == MetricStatus::ok);

    for (std::size_t index = 0; index < current.ticks.size(); ++index) {
        auto previous = zero;
        previous.ticks[index] = current.ticks[index] + 1;
        auto regressed = calculate_cpu(previous, current, 60.0, 80.0);
        CHECK(!regressed);
        CHECK(regressed.error().code == "counter_regression");
    }
    const double invalid_windows[] = {0.0, -1.0,
        std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()};
    for (double window : invalid_windows) {
        CHECK(!calculate_cpu(zero, current, window, 80.0));
    }
    CHECK(!calculate_cpu(zero, zero, 60.0, 80.0));
    CHECK(!calculate_cpu(zero, current, 60.0, 101.0));
    CpuCounters overflowing{{std::numeric_limits<std::uint64_t>::max(), 1, 0, 0, 0, 0, 0, 0}};
    CHECK(!calculate_cpu(zero, overflowing, 60.0, 80.0));

    CpuCounters large_previous;
    CpuCounters large_current;
    for (std::size_t index = 0; index < current.ticks.size(); ++index) {
        large_previous.ticks[index] = std::numeric_limits<std::uint64_t>::max() - 100;
        large_current.ticks[index] = large_previous.ticks[index] + current.ticks[index];
    }
    const auto large = calculate_cpu(large_previous, large_current, 60.0, 80.0);
    CHECK(large);
    CHECK(test::near(*large->usage_percent, 45.0));
}

void memory_tests() {
    const auto result = calculate_memory({1000, 250}, 75.0);
    CHECK(result);
    CHECK(result->total_bytes == 1000);
    CHECK(result->used_bytes == 750);
    CHECK(result->available_bytes == 250);
    CHECK(test::near(*result->usage_percent, 75.0));
    CHECK(result->status == MetricStatus::warning);
    CHECK(calculate_memory({1000, 1000}, 0.0)->status == MetricStatus::warning);
    CHECK(calculate_memory({1000, 0}, 100.0)->status == MetricStatus::warning);
    CHECK(!calculate_memory({0, 0}, 80.0));
    CHECK(!calculate_memory({1000, 1001}, 80.0));
    CHECK(!calculate_memory({1000, 250}, std::numeric_limits<double>::quiet_NaN()));
}

void filesystem_tests() {
    auto sample = filesystem({100, 30, 20, 4096});
    const auto result = calculate_filesystem(sample, 75.0);
    CHECK(result);
    CHECK(result->total_bytes == 409600);
    CHECK(result->used_bytes == 286720);
    CHECK(result->free_bytes == 122880);
    CHECK(result->available_bytes == 81920);
    CHECK(test::near(*result->usage_percent, 100.0 * 70.0 / 90.0));
    CHECK(result->status == MetricStatus::warning);
    CHECK(result->mount_points == sample.mount_points);
    CHECK(result->device_id == "253:1");
    CHECK(*result->used_bytes + *result->available_bytes < *result->total_bytes);
    CHECK(calculate_filesystem(filesystem({100, 100, 90, 4096}), 80.0)->usage_percent == 0.0);
    CHECK(calculate_filesystem(filesystem({100, 10, 0, 4096}), 100.0)->status == MetricStatus::warning);
    const auto empty = calculate_filesystem(filesystem({0, 0, 0, 4096}), 80.0);
    CHECK(empty);
    CHECK(empty->total_bytes == 0);
    CHECK(empty->used_bytes == 0);
    CHECK(empty->free_bytes == 0);
    CHECK(empty->available_bytes == 0);
    CHECK(!empty->usage_percent);
    CHECK(empty->status == MetricStatus::unknown);
    CHECK(!calculate_filesystem(filesystem({100, 30, 20, 0}), 80.0));
    CHECK(!calculate_filesystem(filesystem({100, 101, 20, 4096}), 80.0));
    CHECK(!calculate_filesystem(filesystem({100, 30, 31, 4096}), 80.0));
    const auto reserved = calculate_filesystem(filesystem({100, 100, 0, 4096}), 80.0);
    CHECK(reserved);
    CHECK(reserved->total_bytes == 409600);
    CHECK(reserved->used_bytes == 0);
    CHECK(reserved->free_bytes == 409600);
    CHECK(reserved->available_bytes == 0);
    CHECK(!reserved->usage_percent);
    CHECK(reserved->status == MetricStatus::unknown);
    CHECK(!calculate_filesystem(filesystem({std::numeric_limits<std::uint64_t>::max(), 0, 0, 2}), 80.0));
    sample.counters.value.reset();
    sample.counters.error = Error{"storage", "/data", "permission_denied", "Cannot inspect filesystem"};
    const auto failed = calculate_filesystem(sample, 80.0);
    CHECK(!failed);
    CHECK(failed.error().code == "permission_denied");
}

void threshold_tests() {
    CHECK(validate_thresholds({0.0, 100.0, 80.5}));
    CHECK(!validate_thresholds({-0.1, 80.0, 90.0}));
    CHECK(!validate_thresholds({80.0, 100.1, 90.0}));
    CHECK(!validate_thresholds({80.0, 80.0, std::numeric_limits<double>::infinity()}));
    CHECK(!validate_thresholds({std::numeric_limits<double>::quiet_NaN(), 80.0, 80.0}));
    CHECK(evaluate_threshold(79.999, 80.0) == MetricStatus::ok);
    CHECK(evaluate_threshold(80.0, 80.0) == MetricStatus::warning);
    CHECK(evaluate_threshold(80.001, 80.0) == MetricStatus::warning);
    CHECK(evaluate_threshold(-0.1, 80.0) == MetricStatus::unknown);
    CHECK(evaluate_threshold(100.1, 80.0) == MetricStatus::unknown);
    CHECK(evaluate_threshold(std::numeric_limits<double>::quiet_NaN(), 80.0) == MetricStatus::unknown);
    CHECK(std::string(to_string(ConfigStatus::using_last_valid)) == "using_last_valid");
}

void summary_tests() {
    ResourceSnapshot snapshot;
    snapshot.cpu.status = MetricStatus::ok;
    snapshot.memory.status = MetricStatus::ok;
    snapshot.filesystems.emplace_back();
    snapshot.filesystems.back().status = MetricStatus::ok;
    snapshot.errors.push_back({"thresholds", "", "invalid_json", "Keep last valid policy"});
    snapshot.config_status = ConfigStatus::using_last_valid;
    summarize(snapshot, false);
    CHECK(snapshot.status == MetricStatus::ok);
    CHECK(snapshot.collection_status == CollectionStatus::ok);

    summarize(snapshot, true);
    CHECK(snapshot.status == MetricStatus::unknown);
    CHECK(snapshot.collection_status == CollectionStatus::partial);

    snapshot.cpu.status = MetricStatus::warning;
    snapshot.memory.status = MetricStatus::unknown;
    summarize(snapshot, true);
    CHECK(snapshot.status == MetricStatus::warning);
    CHECK(snapshot.collection_status == CollectionStatus::partial);

    snapshot.cpu.status = MetricStatus::unknown;
    snapshot.filesystems.back().status = MetricStatus::unknown;
    summarize(snapshot, false);
    CHECK(snapshot.status == MetricStatus::unknown);
    CHECK(snapshot.collection_status == CollectionStatus::error);

    snapshot.cpu.status = MetricStatus::ok;
    snapshot.memory.status = MetricStatus::ok;
    snapshot.filesystems.clear();
    summarize(snapshot, false);
    CHECK(snapshot.status == MetricStatus::unknown);
    CHECK(snapshot.collection_status == CollectionStatus::partial);
}

}

int main() {
    return test::run([] {
        cpu_tests();
        memory_tests();
        filesystem_tests();
        threshold_tests();
        summary_tests();
    });
}
