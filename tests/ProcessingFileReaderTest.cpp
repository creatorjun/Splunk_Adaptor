// tests/ProcessingFileReaderTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/linux/ProcessingFileReader.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <pthread.h>
#include <thread>

namespace {

using namespace monitor;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

template <typename Predicate>
void eventually(Predicate condition, std::chrono::milliseconds timeout = 3000ms) {
    const auto end = Clock::now() + timeout;
    while (!condition()) {
        CHECK(Clock::now() < end);
        std::this_thread::sleep_for(1ms);
    }
}

struct Scanner final : IProcessingFileScanner {
    std::atomic<unsigned> calls{};
    std::atomic<unsigned> concurrent{};
    std::atomic<unsigned> maximum{};
    std::atomic<unsigned> cancelled{};
    std::atomic<bool> signals_blocked{};
    std::atomic<bool> block{};
    std::atomic<bool> fail{};
    std::chrono::milliseconds delay{};
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Clock::time_point> starts;
    std::vector<Clock::time_point> finishes;

    ProcessingFilesSnapshot scan(std::stop_token stop) override {
        const auto count = ++calls;
        const auto active = ++concurrent;
        maximum = std::max(maximum.load(), active);
        sigset_t mask;
        ::pthread_sigmask(SIG_SETMASK, nullptr, &mask);
        signals_blocked = ::sigismember(&mask, SIGINT) == 1 && ::sigismember(&mask, SIGTERM) == 1;
        const auto started_at = std::chrono::system_clock::now();
        {
            std::unique_lock lock(mutex);
            starts.push_back(Clock::now());
            std::stop_callback stopped{stop, [this] { wake.notify_all(); }};
            if (block) wake.wait(lock, [this, stop] { return !block || stop.stop_requested(); });
            if (delay.count() > 0) wake.wait_for(lock, delay, [stop] { return stop.stop_requested(); });
            finishes.push_back(Clock::now());
        }
        if (stop.stop_requested()) ++cancelled;
        --concurrent;
        if (fail) throw std::runtime_error("fixture failure");
        ProcessingFilesSnapshot result;
        result.status = ProcessingFilesStatus::ok;
        result.scan_started_at = started_at;
        result.scanned_at = std::chrono::system_clock::now();
        result.directories = {{"fixture", count, 0, {}}};
        return result;
    }

    void release() {
        block = false;
        wake.notify_all();
    }
};

void gate_and_cadence() {
    Scanner scanner;
    ProcessingFileReader reader{scanner, 100ms};
    CHECK(reader.read(false).status == ProcessingFilesStatus::disabled);
    std::this_thread::sleep_for(130ms);
    CHECK(scanner.calls == 0);
    CHECK(reader.read(true).status == ProcessingFilesStatus::pending);
    eventually([&] { return reader.read(true).status == ProcessingFilesStatus::ok; });
    CHECK(scanner.calls == 1);
    CHECK(scanner.signals_blocked);
    for (unsigned count = 0; count < 100; ++count) CHECK(reader.read(true).status == ProcessingFilesStatus::ok);
    CHECK(scanner.calls == 1);
    std::this_thread::sleep_for(240ms);
    CHECK(scanner.calls >= 3);
    CHECK(scanner.maximum == 1);
    CHECK(reader.read(false).status == ProcessingFilesStatus::disabled);
    eventually([&] { return scanner.concurrent == 0; });
    const auto disabled_calls = scanner.calls.load();
    std::this_thread::sleep_for(140ms);
    CHECK(scanner.calls == disabled_calls);
    CHECK(reader.read(true).status == ProcessingFilesStatus::pending);
    eventually([&] { return reader.read(true).status == ProcessingFilesStatus::ok; });
    CHECK(scanner.calls == disabled_calls + 1);
}

void cancellation_and_generation() {
    Scanner scanner;
    scanner.block = true;
    ProcessingFileReader reader{scanner, 200ms};
    CHECK(reader.read(true).status == ProcessingFilesStatus::pending);
    eventually([&] { return scanner.calls == 1; });
    const auto start = Clock::now();
    CHECK(reader.read(false).status == ProcessingFilesStatus::disabled);
    CHECK(Clock::now() - start < 100ms);
    CHECK(reader.read(true).status == ProcessingFilesStatus::pending);
    eventually([&] { return scanner.calls == 2; });
    CHECK(scanner.cancelled == 1);
    CHECK(reader.read(true).status == ProcessingFilesStatus::pending);
    scanner.release();
    ProcessingFilesSnapshot value;
    eventually([&] { value = reader.read(true); return value.status == ProcessingFilesStatus::ok; });
    CHECK(value.directories[0].tmp_count == 2);
    CHECK(scanner.maximum == 1);
}

void stale_and_stop() {
    Scanner scanner;
    {
        ProcessingFileReader reader{scanner, 50ms};
        reader.read(true);
        eventually([&] { return reader.read(true).status == ProcessingFilesStatus::ok; });
        scanner.block = true;
        eventually([&] { return scanner.calls >= 2; });
        std::this_thread::sleep_for(120ms);
        CHECK(reader.read(true).status == ProcessingFilesStatus::pending);
        CHECK(scanner.concurrent == 1);
    }
    CHECK(scanner.cancelled == 1);
    CHECK(scanner.concurrent == 0);
}

void overruns_and_exceptions() {
    Scanner scanner;
    scanner.delay = 125ms;
    {
        ProcessingFileReader reader{scanner, 50ms};
        reader.read(true);
        eventually([&] { return scanner.calls >= 3; });
        reader.read(false);
    }
    CHECK(scanner.maximum == 1);
    CHECK(scanner.starts.size() >= 3);
    CHECK(scanner.starts[1] - scanner.finishes[0] >= 10ms);
    CHECK(scanner.starts[2] - scanner.finishes[1] >= 10ms);
    Scanner failing;
    failing.fail = true;
    ProcessingFileReader reader{failing, 50ms};
    reader.read(true);
    eventually([&] { return reader.read(true).status == ProcessingFilesStatus::error; });
    const auto failure = reader.read(true);
    CHECK(failure.directories.size() == 1);
    CHECK(!failure.directories[0].tmp_count);
    CHECK(!failure.directories[0].done_count);
    CHECK(failure.directories[0].errors.size() == 1);
    CHECK(failure.directories[0].errors[0].component == "processing_files");
    CHECK(failure.directories[0].errors[0].code == "worker_failure");
    failing.fail = false;
    eventually([&] { return reader.read(true).status == ProcessingFilesStatus::ok; });
}

}

int main() {
    return test::run([] {
        gate_and_cadence();
        cancellation_and_generation();
        stale_and_stop();
        overruns_and_exceptions();
    });
}
