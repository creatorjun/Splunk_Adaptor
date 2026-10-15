// src/runtime/DaemonRunner.cpp
#include "runtime/DaemonRunner.hpp"

#include "infrastructure/linux/UniqueFd.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>

namespace monitor {
namespace {

class SignalMask {
public:
    SignalMask() {
        ::sigemptyset(&mask_);
        ::sigaddset(&mask_, SIGTERM);
        ::sigaddset(&mask_, SIGINT);
        if (::sigprocmask(SIG_BLOCK, &mask_, &previous_) != 0) {
            throw std::runtime_error("sigprocmask: " + std::string(std::strerror(errno)));
        }
    }
    ~SignalMask() { ::sigprocmask(SIG_SETMASK, &previous_, nullptr); }
    SignalMask(const SignalMask&) = delete;
    SignalMask& operator=(const SignalMask&) = delete;
    const sigset_t* get() const noexcept { return &mask_; }

private:
    sigset_t mask_{};
    sigset_t previous_{};
};

std::string safe_log_text(const std::string& text) {
    std::string result;
    for (const unsigned char character : text) {
        result += character < 32 || character == 127 ? '?' : static_cast<char>(character);
    }
    return result;
}

int system_failure(const char* operation) {
    const int number = errno;
    log_error({"runtime", operation, std::to_string(number), std::strerror(number)});
    return 1;
}

}

void log_error(const Error& error) {
    std::cerr << "component=" << safe_log_text(error.component)
              << " target=" << safe_log_text(error.target)
              << " code=" << safe_log_text(error.code)
              << " message=" << safe_log_text(error.message) << '\n';
}

int run_daemon(CollectAndStore& use_case, const DaemonOptions& options) {
    SignalMask signal_mask;
    UniqueFd signals(::signalfd(-1, signal_mask.get(), SFD_CLOEXEC | SFD_NONBLOCK));
    if (!signals) {
        return system_failure("signalfd");
    }
    UniqueFd directory(::open(options.output_directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!directory) {
        return system_failure("open output directory");
    }
    UniqueFd lock(::openat(directory.get(), ".resource-monitor.lock", O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0640));
    if (!lock) {
        return system_failure("open lock file");
    }
    struct stat lock_stat{};
    if (::fstat(lock.get(), &lock_stat) != 0) {
        return system_failure("stat lock file");
    }
    if (!S_ISREG(lock_stat.st_mode) || lock_stat.st_nlink != 1) {
        log_error({"runtime", ".resource-monitor.lock", "invalid_lock", "Lock must be a regular file with one link"});
        return 1;
    }
    if (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
        return system_failure("acquire single-instance lock");
    }
    const auto initialized = use_case.initialize();
    if (!initialized) {
        log_error(initialized.error());
        return 78;
    }
    UniqueFd timer(::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK));
    if (!timer) {
        return system_failure("timerfd_create");
    }
    itimerspec schedule{};
    schedule.it_value.tv_sec = options.interval.count();
    schedule.it_interval.tv_sec = options.interval.count();
    if (::timerfd_settime(timer.get(), 0, &schedule, nullptr) != 0) {
        return system_failure("timerfd_settime");
    }
    std::cout << "resource-monitor started interval_seconds=" << options.interval.count()
              << " output=" << safe_log_text(options.output_directory.string()) << std::endl;
    pollfd descriptors[]{{signals.get(), POLLIN, 0}, {timer.get(), POLLIN, 0}};
    std::uint64_t skipped_intervals{};
    for (;;) {
        const int ready = ::poll(descriptors, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return system_failure("poll");
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            signalfd_siginfo signal{};
            if (::read(signals.get(), &signal, sizeof(signal)) == sizeof(signal)) {
                std::cout << "resource-monitor stopped signal=" << signal.ssi_signo << std::endl;
                return 0;
            }
        }
        if ((descriptors[0].revents | descriptors[1].revents) & (POLLERR | POLLHUP | POLLNVAL)) {
            log_error({"runtime", "poll", "descriptor_failure", "Event descriptor failed"});
            return 1;
        }
        if ((descriptors[1].revents & POLLIN) == 0) {
            continue;
        }
        std::uint64_t expirations{};
        if (::read(timer.get(), &expirations, sizeof(expirations)) != sizeof(expirations)) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            return system_failure("read timer");
        }
        const auto result = use_case.run(skipped_intervals + (expirations > 0 ? expirations - 1 : 0));
        skipped_intervals = 0;
        if (!result) {
            log_error(result.error());
            if (options.once) {
                return 1;
            }
        } else {
            for (const auto& error : result->errors) {
                log_error(error);
            }
            std::cout << (result->status == MetricStatus::warning ? "snapshot_saved" : "snapshot_skipped")
                      << " status=" << to_string(result->status)
                      << " collection_status=" << to_string(result->collection_status)
                      << " config_status=" << to_string(result->config_status) << std::endl;
            if (options.once) {
                return 0;
            }
        }
        std::uint64_t elapsed_during_collection{};
        ssize_t drained{};
        do {
            drained = ::read(timer.get(), &elapsed_during_collection, sizeof(elapsed_during_collection));
        } while (drained < 0 && errno == EINTR);
        if (drained == sizeof(elapsed_during_collection)) {
            skipped_intervals = elapsed_during_collection;
        } else if (drained < 0 && errno != EAGAIN) {
            return system_failure("drain missed timer intervals");
        }
    }
}

}
