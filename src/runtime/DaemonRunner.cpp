// src/runtime/DaemonRunner.cpp
#include "runtime/DaemonRunner.hpp"

#include "application/SettingsController.hpp"
#include "infrastructure/linux/UniqueFd.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

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

int run_daemon(CollectAndStore& use_case, const DaemonOptions& options,
    SettingsController* settings) {
    SignalMask signal_mask;
    UniqueFd signals(::signalfd(-1, signal_mask.get(), SFD_CLOEXEC | SFD_NONBLOCK));
    if (!signals) {
        return system_failure("signalfd");
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
    auto interval = settings ? settings->current().collection_interval : options.interval;
    const auto arm_timer = [](int descriptor, std::chrono::seconds seconds) {
        itimerspec schedule{};
        schedule.it_value.tv_sec = seconds.count();
        schedule.it_interval.tv_sec = seconds.count();
        return ::timerfd_settime(descriptor, 0, &schedule, nullptr);
    };
    if (arm_timer(timer.get(), interval) != 0) {
        return system_failure("timerfd_settime");
    }
    UniqueFd reload_timer;
    if (settings) {
        reload_timer.reset(::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK));
        if (!reload_timer) {
            return system_failure("create settings timer");
        }
        if (arm_timer(reload_timer.get(), std::chrono::seconds{1}) != 0) {
            return system_failure("arm settings timer");
        }
    }
    std::cout << "resource-monitor started interval_seconds=" << interval.count()
              << " output=" << safe_log_text(settings ? settings->current().output_directory
                  : options.output_directory.string()) << std::endl;
    pollfd descriptors[]{{signals.get(), POLLIN, 0}, {timer.get(), POLLIN, 0},
        {reload_timer.get(), POLLIN, 0}};
    std::uint64_t skipped_intervals{};
    std::optional<Error> reload_error;
    for (;;) {
        const int ready = ::poll(descriptors, 3, -1);
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
        if ((descriptors[0].revents | descriptors[1].revents | descriptors[2].revents) &
            (POLLERR | POLLHUP | POLLNVAL)) {
            log_error({"runtime", "poll", "descriptor_failure", "Event descriptor failed"});
            return 1;
        }
        if (settings && (descriptors[2].revents & POLLIN) != 0) {
            std::uint64_t checks{};
            ssize_t count{};
            do {
                count = ::read(reload_timer.get(), &checks, sizeof(checks));
            } while (count < 0 && errno == EINTR);
            if (count == sizeof(checks)) {
                const auto changed = settings->reload();
                if (!changed) {
                    const auto& error = changed.error();
                    if (!reload_error || reload_error->component != error.component ||
                        reload_error->target != error.target || reload_error->code != error.code ||
                        reload_error->message != error.message) {
                        log_error(error);
                    }
                    reload_error = error;
                } else {
                    const bool recovered = reload_error.has_value();
                    reload_error.reset();
                    const auto& current = settings->current();
                    const bool interval_changed = interval != current.collection_interval;
                    if (interval_changed) {
                        if (arm_timer(timer.get(), current.collection_interval) != 0) {
                            return system_failure("rearm collection timer");
                        }
                        interval = current.collection_interval;
                        skipped_intervals = 0;
                    }
                    if (*changed || recovered) {
                        std::cout << (*changed ? "settings_reloaded" : "settings_recovered")
                                  << " interval_seconds=" << interval.count()
                                  << " output=" << safe_log_text(current.output_directory) << std::endl;
                    }
                    if (interval_changed) {
                        continue;
                    }
                }
            } else if (count >= 0 || errno != EAGAIN) {
                return system_failure("read settings timer");
            }
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
            std::cout << "snapshot_saved status=" << to_string(result->status)
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
