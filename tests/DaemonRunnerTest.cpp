// tests/DaemonRunnerTest.cpp
#include "TestSupport.hpp"
#include "application/SettingsController.hpp"
#include "application/SnapshotRetention.hpp"
#include "infrastructure/linux/UniqueFd.hpp"
#include "runtime/DaemonRunner.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace monitor;
using namespace std::chrono_literals;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        char pattern[] = "/tmp/resource-monitor-runtime-XXXXXX";
        const auto* created = ::mkdtemp(pattern);
        if (created == nullptr) {
            throw std::runtime_error("Unable to create runtime test directory");
        }
        path_ = created;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

class ChildProcess {
public:
    explicit ChildProcess(pid_t child) noexcept : child_(child) {}
    ~ChildProcess() {
        if (child_ > 0) {
            ::kill(child_, SIGKILL);
            int status{};
            while (::waitpid(child_, &status, 0) < 0 && errno == EINTR) {}
        }
    }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    int stop() {
        CHECK(::kill(child_, SIGTERM) == 0);
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            int status{};
            const auto result = ::waitpid(child_, &status, WNOHANG);
            if (result == child_) {
                child_ = -1;
                return status;
            }
            if (result < 0 && errno != EINTR) {
                if (errno == ECHILD) {
                    child_ = -1;
                }
                throw std::runtime_error("Unable to wait for runtime test process");
            }
            std::this_thread::sleep_for(10ms);
        }
        throw std::runtime_error("Runtime did not stop promptly after SIGTERM");
    }

private:
    pid_t child_;
};

struct SavedRecord {
    std::int64_t monotonic_nanoseconds{};
    std::uint64_t missed_intervals{};
};

struct AnalysisReloadRecord {
    std::int64_t interval_seconds{};
    std::size_t saved_snapshots{};
};

class FixtureReader final : public IResourceReader {
public:
    explicit FixtureReader(std::chrono::milliseconds delay = 0ms, bool cpu_available = true)
        : delay_(delay), cpu_available_(cpu_available) {}

    ResourceSample read() override {
        if (reads_++ != 0) {
            std::this_thread::sleep_for(delay_);
        }
        ResourceSample result;
        result.timestamp = std::chrono::system_clock::now();
        result.cpu_observed_at = std::chrono::steady_clock::now();
        result.hostname = "runtime-test";
        if (cpu_available_) {
            result.cpu.value = CpuCounters{{reads_ * 50, 0, 0, reads_ * 50, 0, 0, 0, 0}};
        } else {
            result.cpu.error = Error{"cpu", "/proc/stat", "fixture_unavailable", "CPU counters unavailable"};
        }
        result.memory.value = MemoryCounters{1000, 500};
        FilesystemSample filesystem;
        filesystem.device = "/dev/test";
        filesystem.device_id = "1:1";
        filesystem.filesystem_type = "xfs";
        filesystem.mount_points = {"/test"};
        filesystem.counters.value = FilesystemCounters{100, 50, 50, 4096};
        result.filesystems.push_back(filesystem);
        return result;
    }

private:
    std::uint64_t reads_{};
    std::chrono::milliseconds delay_;
    bool cpu_available_;
};

class FixedConfig final : public IThresholdConfigReader {
public:
    explicit FixedConfig(ThresholdPolicy policy = {80.0, 80.0, 80.0}) : policy_(policy) {}

    std::expected<ThresholdPolicy, Error> read() override {
        return policy_;
    }

private:
    ThresholdPolicy policy_;
};

class RecordingStore final : public ISnapshotStore {
public:
    std::expected<void, Error> save(const ResourceSnapshot&) override {
        ++calls;
        if (fail) {
            return std::unexpected(Error{"store", "fixture", "fixture_save_failed", "Unable to save snapshot"});
        }
        return {};
    }

    std::size_t calls{};
    bool fail{};
};

class ReloadingSettings final : public ISettingsReader {
public:
    std::expected<MonitorSettings, Error> read_settings() override {
        MonitorSettings settings;
        settings.collection_interval = reads_++ == 0 ? 60s : 86400s;
        return settings;
    }

private:
    unsigned reads_{};
};

class FixtureOutput final : public IOutputDirectory {
public:
    std::expected<void, Error> configure_output(const std::string&) override { return {}; }
};

class ReloadRecordingMonitor final : public ICollectCountMonitor {
public:
    ReloadRecordingMonitor(const SettingsController& settings, const RecordingStore& store, int descriptor)
        : settings_(settings), store_(store), descriptor_(descriptor) {}

    CollectCountAnalysis collect(std::chrono::system_clock::time_point) override { return {}; }

    void settings_changed(std::chrono::system_clock::time_point) override {
        const AnalysisReloadRecord record{settings_.current().collection_interval.count(), store_.calls};
        CHECK(::write(descriptor_, &record, sizeof(record)) == static_cast<ssize_t>(sizeof(record)));
    }

private:
    const SettingsController& settings_;
    const RecordingStore& store_;
    int descriptor_;
};

class CapturedLogs {
public:
    CapturedLogs() : previous_output_(std::cout.rdbuf(output.rdbuf())), previous_errors_(std::cerr.rdbuf(errors.rdbuf())) {}
    ~CapturedLogs() {
        std::cout.rdbuf(previous_output_);
        std::cerr.rdbuf(previous_errors_);
    }
    CapturedLogs(const CapturedLogs&) = delete;
    CapturedLogs& operator=(const CapturedLogs&) = delete;

    std::ostringstream output;
    std::ostringstream errors;

private:
    std::streambuf* previous_output_;
    std::streambuf* previous_errors_;
};

class PipeStore final : public ISnapshotStore {
public:
    explicit PipeStore(int descriptor) noexcept : descriptor_(descriptor) {}

    std::expected<void, Error> save(const ResourceSnapshot& snapshot) override {
        const SavedRecord record{
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
            snapshot.missed_intervals
        };
        const auto* data = reinterpret_cast<const char*>(&record);
        std::size_t written{};
        while (written < sizeof(record)) {
            const auto amount = ::write(descriptor_, data + written, sizeof(record) - written);
            if (amount < 0 && errno == EINTR) {
                continue;
            }
            if (amount <= 0) {
                return std::unexpected(Error{"test_store", "pipe", "write_failed",
                    "Unable to report saved runtime sample"});
            }
            written += static_cast<std::size_t>(amount);
        }
        return {};
    }

private:
    int descriptor_;
};

template <typename Record = SavedRecord>
Record read_record(int descriptor) {
    Record record;
    auto* data = reinterpret_cast<char*>(&record);
    std::size_t received{};
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (received < sizeof(record)) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= 0s) {
            throw std::runtime_error("Timed out waiting for a runtime sample");
        }
        const auto timeout = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count());
        pollfd event{descriptor, POLLIN, 0};
        const auto ready = ::poll(&event, 1, timeout);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        CHECK(ready >= 0);
        if (ready == 0) {
            continue;
        }
        if ((event.revents & POLLIN) == 0) {
            throw std::runtime_error("Runtime sample pipe closed before reporting a result");
        }
        const auto amount = ::read(descriptor, data + received, sizeof(record) - received);
        if (amount < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
        CHECK(amount > 0);
        received += static_cast<std::size_t>(amount);
    }
    return record;
}

void runtime_overrun_test() {
    TemporaryDirectory directory;
    int descriptors[2]{};
    CHECK(::pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) == 0);
    UniqueFd reading(descriptors[0]);
    UniqueFd writing(descriptors[1]);
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        reading.reset();
        try {
            FixtureReader reader(1200ms);
            PipeStore store(writing.get());
            FixedConfig config;
            CollectAndStore use_case(reader, store, config);
            DaemonOptions options;
            options.output_directory = directory.path();
            options.interval = 1s;
            const int result = run_daemon(use_case, options);
            ::_exit(result);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            ::_exit(1);
        }
    }
    ChildProcess process(child);
    writing.reset();
    const auto first = read_record(reading.get());
    const auto second = read_record(reading.get());
    const double elapsed = static_cast<double>(
        second.monotonic_nanoseconds - first.monotonic_nanoseconds) / 1000000000.0;
    CHECK(elapsed >= 1.7);
    CHECK(second.missed_intervals >= 1);
    const auto status = process.stop();
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

void once_status_tests() {
    const auto run_case = [](double threshold, bool cpu_available, bool fail_save,
                             const std::string& expected_status, int expected_exit) {
        TemporaryDirectory directory;
        FixtureReader reader(0ms, cpu_available);
        FixedConfig config({threshold, threshold, threshold});
        RecordingStore store;
        store.fail = fail_save;
        CollectAndStore use_case(reader, store, config);
        DaemonOptions options;
        options.output_directory = directory.path();
        options.interval = 1s;
        options.once = true;
        CapturedLogs logs;
        CHECK(run_daemon(use_case, options) == expected_exit);
        CHECK(store.calls == 1);
        CHECK(logs.output.str().find("snapshot_skipped") == std::string::npos);
        if (expected_exit == 0) {
            CHECK(logs.output.str().find("snapshot_saved status=" + expected_status) != std::string::npos);
        } else {
            CHECK(logs.output.str().find("snapshot_saved") == std::string::npos);
            CHECK(logs.errors.str().find("fixture_save_failed") != std::string::npos);
        }
        if (!cpu_available) {
            CHECK(logs.errors.str().find("fixture_unavailable") != std::string::npos);
        }
    };
    run_case(80.0, true, false, "ok", 0);
    run_case(80.0, false, false, "unknown", 0);
    run_case(50.0, true, false, "warning", 0);
    run_case(80.0, true, true, "", 1);
    run_case(50.0, true, true, "", 1);
}

void analysis_reload_before_collection_test() {
    int descriptors[2]{};
    CHECK(::pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) == 0);
    UniqueFd reading(descriptors[0]);
    UniqueFd writing(descriptors[1]);
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        reading.reset();
        try {
            ReloadingSettings settings_reader;
            FixtureOutput output;
            SettingsController settings(settings_reader, output);
            CHECK(settings.initialize());
            CHECK(settings.current().collection_interval == 60s);
            FixtureReader reader;
            RecordingStore store;
            ReloadRecordingMonitor monitor(settings, store, writing.get());
            CollectAndStore use_case(reader, store, settings, &monitor);
            const int result = run_daemon(use_case, {}, &settings);
            ::_exit(result);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            ::_exit(1);
        }
    }
    ChildProcess process(child);
    writing.reset();
    const auto notification = read_record<AnalysisReloadRecord>(reading.get());
    CHECK(notification.interval_seconds == 86400);
    CHECK(notification.saved_snapshots == 0);
    const auto status = process.stop();
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

void retention_failure_keeps_collection_test() {
    class FailingRetention final : public ISnapshotRetention {
    public:
        std::expected<std::size_t, Error> prune(std::chrono::system_clock::time_point cutoff) override {
            ++calls;
            observed_cutoff = cutoff;
            if (throw_error) throw std::runtime_error("fixture retention exception");
            return std::unexpected(Error{"snapshot_retention", "fixture", "fixture_cleanup_failed",
                "Unable to remove expired fixture"});
        }
        unsigned calls{};
        bool throw_error{};
        std::chrono::system_clock::time_point observed_cutoff;
    };
    for (const bool throw_error : {false, true}) {
        FixtureReader reader;
        FixedConfig config;
        RecordingStore store;
        CollectAndStore use_case(reader, store, config);
        FailingRetention storage;
        storage.throw_error = throw_error;
        SnapshotRetention retention(storage);
        const auto invalid_clock = retention.run(std::chrono::system_clock::time_point::min());
        CHECK(!invalid_clock && invalid_clock.error().code == "invalid_timestamp");
        CHECK(storage.calls == 0);
        DaemonOptions options;
        options.interval = 1s;
        options.once = true;
        CapturedLogs logs;
        const auto started = std::chrono::system_clock::now();
        CHECK(run_daemon(use_case, options, nullptr, &retention) == 0);
        CHECK(storage.calls == 1);
        CHECK(storage.observed_cutoff >= started - 1h);
        CHECK(storage.observed_cutoff <= std::chrono::system_clock::now() - 1h);
        CHECK(store.calls == 1);
        CHECK(logs.errors.str().find("component=snapshot_retention") != std::string::npos);
        CHECK(logs.output.str().find("snapshot_saved") != std::string::npos);
    }
}

}

int main() {
    return test::run([] {
        runtime_overrun_test();
        once_status_tests();
        analysis_reload_before_collection_test();
        retention_failure_keeps_collection_test();
    });
}
