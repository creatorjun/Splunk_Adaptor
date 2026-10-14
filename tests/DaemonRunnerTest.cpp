// tests/DaemonRunnerTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/linux/UniqueFd.hpp"
#include "runtime/DaemonRunner.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
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

class SlowReader final : public IResourceReader {
public:
    ResourceSample read() override {
        if (reads_++ != 0) {
            std::this_thread::sleep_for(1200ms);
        }
        ResourceSample result;
        result.timestamp = std::chrono::system_clock::now();
        result.cpu_observed_at = std::chrono::steady_clock::now();
        result.hostname = "runtime-test";
        result.cpu.value = CpuCounters{{reads_ * 50, 0, 0, reads_ * 50, 0, 0, 0, 0}};
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
};

class FixedConfig final : public IThresholdConfigReader {
public:
    std::expected<ThresholdPolicy, Error> read() override {
        return ThresholdPolicy{80.0, 80.0, 80.0};
    }
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

SavedRecord read_record(int descriptor) {
    SavedRecord record;
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
            SlowReader reader;
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

}

int main() {
    return test::run(runtime_overrun_test);
}
