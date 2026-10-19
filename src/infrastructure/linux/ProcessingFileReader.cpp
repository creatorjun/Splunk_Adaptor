// src/infrastructure/linux/ProcessingFileReader.cpp
#include "infrastructure/linux/ProcessingFileReader.hpp"

#include <condition_variable>
#include <csignal>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <thread>

namespace monitor {
namespace {

using Clock = std::chrono::steady_clock;

class InheritedSignalMask {
public:
    InheritedSignalMask() {
        sigset_t mask;
        ::sigemptyset(&mask);
        ::sigaddset(&mask, SIGTERM);
        ::sigaddset(&mask, SIGINT);
        if (::pthread_sigmask(SIG_BLOCK, &mask, &previous_) != 0) {
            throw std::runtime_error("Unable to mask processing file worker signals");
        }
    }
    ~InheritedSignalMask() { ::pthread_sigmask(SIG_SETMASK, &previous_, nullptr); }
    InheritedSignalMask(const InheritedSignalMask&) = delete;
    InheritedSignalMask& operator=(const InheritedSignalMask&) = delete;

private:
    sigset_t previous_{};
};

}

class ProcessingFileReader::State {
public:
    State(IProcessingFileScanner& source, std::chrono::milliseconds period)
        : scanner(source), interval(period) {
        if (interval.count() <= 0 || interval > std::chrono::hours{24}) {
            throw std::invalid_argument("Processing file scan interval must be positive and at most one day");
        }
        InheritedSignalMask mask;
        worker = std::thread([this] { run(); });
    }

    ~State() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            active_stop.request_stop();
        }
        wake.notify_one();
        worker.join();
    }

    ProcessingFilesSnapshot read(bool requested) {
        std::lock_guard lock(mutex);
        if (enabled != requested) {
            enabled = requested;
            ++generation;
            active_stop.request_stop();
            cached = {};
            completed_at.reset();
            next_scan = Clock::now();
            wake.notify_one();
        }
        if (!enabled) return {};
        if (!completed_at || Clock::now() - *completed_at > interval * 2) {
            ProcessingFilesSnapshot pending;
            pending.status = ProcessingFilesStatus::pending;
            return pending;
        }
        return cached;
    }

private:
    void run() {
        std::unique_lock lock(mutex);
        while (!stopping) {
            if (!enabled) {
                wake.wait(lock, [this] { return stopping || enabled; });
                continue;
            }
            const auto revision = generation;
            if (Clock::now() < next_scan) {
                wake.wait_until(lock, next_scan,
                    [this, revision] { return stopping || !enabled || generation != revision; });
                continue;
            }
            const auto scheduled_start = next_scan;
            active_stop = std::stop_source{};
            const auto stop = active_stop.get_token();
            lock.unlock();
            ProcessingFilesSnapshot result;
            const auto started = std::chrono::system_clock::now();
            try {
                result = scanner.scan(stop);
            } catch (...) {
                result.status = ProcessingFilesStatus::error;
                result.scan_started_at = started;
                result.scanned_at = std::chrono::system_clock::now();
                result.directories = {{"processing_files", std::nullopt, std::nullopt,
                    {{"processing_files", "scan", "worker_failure", "Processing directory scan failed"}}}};
            }
            const auto finished = Clock::now();
            lock.lock();
            if (stopping) break;
            if (!enabled || generation != revision || stop.stop_requested()) continue;
            cached = std::move(result);
            completed_at = finished;
            const auto periods = (finished - scheduled_start) / interval + 1;
            next_scan = scheduled_start + interval * periods;
        }
    }

    IProcessingFileScanner& scanner;
    std::chrono::milliseconds interval;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping{};
    bool enabled{};
    std::uint64_t generation{};
    std::stop_source active_stop;
    ProcessingFilesSnapshot cached;
    std::optional<Clock::time_point> completed_at;
    Clock::time_point next_scan{};
    std::thread worker;
};

ProcessingFileReader::ProcessingFileReader(IProcessingFileScanner& scanner,
    std::chrono::milliseconds interval)
    : state_(std::make_unique<State>(scanner, interval)) {}

ProcessingFileReader::~ProcessingFileReader() = default;

ProcessingFilesSnapshot ProcessingFileReader::read(bool enabled) { return state_->read(enabled); }

}
