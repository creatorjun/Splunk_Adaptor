// src/infrastructure/database/MySqlConfiguration.cpp
#include "infrastructure/database/MySqlConfiguration.hpp"

#include <algorithm>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace monitor {
namespace {

using Clock = std::chrono::steady_clock;

struct Identity {
    MySqlSettings settings;
    MySqlCredentials credentials;

    bool operator==(const Identity&) const = default;
};

struct Request {
    std::uint64_t generation;
    Identity identity;
};

class InheritedSignalMask {
public:
    InheritedSignalMask() {
        sigset_t mask;
        ::sigemptyset(&mask);
        ::sigaddset(&mask, SIGTERM);
        ::sigaddset(&mask, SIGINT);
        if (::pthread_sigmask(SIG_BLOCK, &mask, &previous_) != 0) {
            throw std::runtime_error("Unable to mask database worker signals");
        }
    }
    ~InheritedSignalMask() { ::pthread_sigmask(SIG_SETMASK, &previous_, nullptr); }
    InheritedSignalMask(const InheritedSignalMask&) = delete;
    InheritedSignalMask& operator=(const InheritedSignalMask&) = delete;

private:
    sigset_t previous_{};
};

Error worker_error() {
    return {"mysql", "connection", "worker_failure", "Database connection operation failed"};
}

}

class MySqlConfiguration::State {
public:
    State(IMySqlCredentialsReader& reader, IMySqlConnectionFactory& connector,
        std::chrono::milliseconds retry, std::chrono::milliseconds health, std::chrono::milliseconds query_period)
        : credentials(reader), factory(connector), retry_interval(retry), health_interval(health), query_interval(query_period) {
        if (retry.count() <= 0 || health.count() <= 0 || query_period.count() <= 0) {
            throw std::invalid_argument("Database retry, health, and query intervals must be positive");
        }
        retry_delay = std::min(retry_interval, maximum_retry_interval);
        InheritedSignalMask mask;
        worker = std::thread([this] { run(); });
    }

    ~State() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        worker.join();
    }

    void invalidate_ready() {
        if (ready) {
            retired.push_back(std::move(ready));
            wake.notify_one();
        }
        ready_generation.reset();
        prepared_generation.reset();
        prepared_identity.reset();
    }

    std::expected<bool, Error> prepare(const MySqlSettings& settings) {
        Identity identity{settings, {}};
        if (settings.enabled) {
            auto value = credentials.read_credentials(settings.credentials_file);
            if (!value) {
                std::lock_guard lock(mutex);
                invalidate_ready();
                desired.reset();
                volume_window.reset();
                volume_result = {};
                ++volume_revision;
                request.reset();
                ++generation;
                failure = value.error();
                retry_delay = std::min(retry_interval, maximum_retry_interval);
                return std::unexpected(value.error());
            }
            identity.credentials = std::move(*value);
        }
        std::lock_guard lock(mutex);
        prepared_generation.reset();
        if (!desired || *desired != identity) {
            invalidate_ready();
            desired = std::move(identity);
            volume_window.reset();
            volume_result = {};
            ++volume_revision;
            ++generation;
            request.reset();
            failure.reset();
            retry_at = Clock::time_point{};
            retry_delay = std::min(retry_interval, maximum_retry_interval);
        }
        const bool active_matches = active_identity && *active_identity == *desired && active_healthy;
        if (!active_matches && ready && ready_generation == generation && Clock::now() >= ready_expires_at) {
            invalidate_ready();
        }
        if (!settings.enabled || active_matches
            || (ready && ready_generation == generation)) {
            prepared_identity = *desired;
            prepared_generation = generation;
            retired.reserve(retired.size() + 1);
            return true;
        }
        if (failure && Clock::now() < retry_at) {
            return std::unexpected(*failure);
        }
        if (!request && working_generation != generation) {
            request = Request{generation, *desired};
            wake.notify_one();
        }
        if (failure) return std::unexpected(*failure);
        return false;
    }

    bool commit() noexcept {
        std::lock_guard lock(mutex);
        if (!prepared_generation || *prepared_generation != generation || !prepared_identity) {
            return false;
        }
        prepared_generation.reset();
        if (active_identity && *active_identity == *prepared_identity && active_healthy) {
            prepared_identity.reset();
            return false;
        }
        if (prepared_identity->settings.enabled && (!ready || ready_generation != generation)) {
            return false;
        }
        const bool same_identity = active_identity && *active_identity == *prepared_identity;
        if (active) {
            retired.push_back(std::move(active));
        }
        if (prepared_identity->settings.enabled) {
            active = std::move(ready);
        }
        active_identity.swap(prepared_identity);
        prepared_identity.reset();
        ready_generation.reset();
        active_healthy = true;
        ++active_revision;
        ++volume_revision;
        if (!same_identity || !active_identity->settings.enabled) {
            volume_result = {};
            volume_at = Clock::time_point{};
        }
        if (!active_identity->settings.enabled) {
            volume_window.reset();
        }
        failure.reset();
        retry_delay = std::min(retry_interval, maximum_retry_interval);
        health_at = Clock::now() + health_interval;
        wake.notify_one();
        return true;
    }

    LogVolumeReading read_log_volume(const LogVolumeWindow& window) {
        std::lock_guard lock(mutex);
        if (!volume_window || *volume_window != window) {
            volume_window = window;
            ++volume_revision;
            volume_result = {};
            volume_result.window = window;
            volume_at = Clock::time_point{};
            wake.notify_one();
        }
        if (!active || !active_healthy || !desired || !active_identity ||
            !desired->settings.enabled || *active_identity != *desired) {
            LogVolumeReading result;
            result.window = window;
            result.queried_at = volume_result.queried_at;
            if (volume_result.error || failure || (active && !active_healthy && desired && desired->settings.enabled)) {
                result.status = LogVolumeReadStatus::error;
                result.error = volume_result.error ? volume_result.error : failure;
                if (!result.error) result.error = Error{"log_volume", "connection", "connection_unavailable",
                    "Database log volume connection is unavailable"};
            }
            return result;
        }
        return volume_result;
    }

    void run() {
        std::unique_lock lock(mutex);
        while (!stopping) {
            if (!retired.empty()) {
                auto discarded = std::move(retired.back());
                retired.pop_back();
                lock.unlock();
                discarded.reset();
                lock.lock();
                continue;
            }
            if (request) {
                auto task = std::move(*request);
                request.reset();
                working_generation = task.generation;
                lock.unlock();
                auto connected = connect(task);
                lock.lock();
                working_generation.reset();
                if (!stopping && generation == task.generation && desired && *desired == task.identity) {
                    if (connected) {
                        ready = std::move(*connected);
                        ready_generation = task.generation;
                        ready_expires_at = Clock::now() + health_interval;
                        retry_delay = std::min(retry_interval, maximum_retry_interval);
                    } else {
                        if (!failure) failure = connected.error();
                        retry_at = Clock::now() + retry_delay;
                        retry_delay = std::min(retry_delay * 2, maximum_retry_interval);
                    }
                }
                lock.unlock();
                connected = std::unexpected(worker_error());
                lock.lock();
                continue;
            }
            if (active && active_healthy && active_identity && desired && *active_identity == *desired &&
                desired->settings.enabled && Clock::now() >= health_at) {
                auto session = active;
                const auto revision = active_revision;
                lock.unlock();
                auto result = probe(*session);
                lock.lock();
                if (revision == active_revision) {
                    health_at = Clock::now() + health_interval;
                    if (!result) {
                        active_healthy = false;
                        if (active_identity && desired && *active_identity == *desired) {
                            if (!failure) failure = volume_result.error ? *volume_result.error : result.error();
                            retry_at = Clock::now();
                        }
                    }
                }
                lock.unlock();
                session.reset();
                lock.lock();
                continue;
            }
            if (active && active_healthy && active_identity && desired && *active_identity == *desired &&
                desired->settings.enabled && volume_window && Clock::now() >= volume_at && Clock::now() >= next_query_at) {
                auto session = active;
                const auto revision = active_revision;
                const auto selected_revision = volume_revision;
                const auto window = *volume_window;
                lock.unlock();
                auto result = query_volume(*session, window);
                const auto queried_at = std::chrono::system_clock::now();
                lock.lock();
                if (!stopping && revision == active_revision && selected_revision == volume_revision
                    && volume_window && *volume_window == window) {
                    volume_result.window = window;
                    if (result) {
                        volume_result.status = LogVolumeReadStatus::ready;
                        volume_result.equipment = std::move(*result);
                        volume_result.error.reset();
                        volume_result.queried_at = queried_at;
                        volume_at = Clock::time_point::max();
                        next_query_at = Clock::time_point{};
                    } else {
                        volume_result.status = LogVolumeReadStatus::error;
                        volume_result.equipment.clear();
                        if (!volume_result.error) {
                            volume_result.error = std::move(result.error());
                            volume_result.queried_at = queried_at;
                        }
                        next_query_at = Clock::now() + query_interval;
                        volume_at = next_query_at;
                        health_at = Clock::now();
                    }
                }
                lock.unlock();
                session.reset();
                lock.lock();
                continue;
            }
            if (active && active_healthy && active_identity && desired && *active_identity == *desired &&
                desired->settings.enabled) {
                auto deadline = health_at;
                if (volume_window && active_identity && desired && *active_identity == *desired && desired->settings.enabled) {
                    deadline = std::min(deadline, std::max(volume_at, next_query_at));
                }
                wake.wait_until(lock, deadline);
            } else {
                wake.wait(lock);
            }
        }
        auto discarded_active = std::move(active);
        auto discarded_ready = std::move(ready);
        auto discarded_retired = std::move(retired);
        lock.unlock();
    }

    std::expected<std::shared_ptr<IMySqlSession>, Error> connect(const Request& task) {
        try {
            auto result = factory.connect(task.identity.settings, task.identity.credentials);
            if (!result) {
                return std::unexpected(result.error());
            }
            if (!*result) {
                return std::unexpected(worker_error());
            }
            {
                std::lock_guard lock(mutex);
                if (stopping || generation != task.generation || !desired || *desired != task.identity) {
                    return std::unexpected(worker_error());
                }
            }
            auto verified = (*result)->probe();
            if (!verified) {
                return std::unexpected(verified.error());
            }
            return std::shared_ptr<IMySqlSession>(std::move(*result));
        } catch (...) {
            return std::unexpected(worker_error());
        }
    }

    static std::expected<void, Error> probe(IMySqlSession& session) {
        try {
            return session.probe();
        } catch (...) {
            return std::unexpected(worker_error());
        }
    }

    static std::expected<std::vector<EquipmentLogVolume>, Error> query_volume(
        IMySqlSession& session, const LogVolumeWindow& window) {
        try {
            return session.read_log_volume(window);
        } catch (...) {
            return std::unexpected(Error{"log_volume", "query", "query_failure",
                "Database log volume query failed"});
        }
    }

    IMySqlCredentialsReader& credentials;
    IMySqlConnectionFactory& factory;
    std::chrono::milliseconds retry_interval;
    std::chrono::milliseconds health_interval;
    std::chrono::milliseconds query_interval;
    Clock::time_point next_query_at{};
    static constexpr std::chrono::milliseconds maximum_retry_interval{60000};
    std::chrono::milliseconds retry_delay{};
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping{};
    std::thread worker;
    std::uint64_t generation{};
    std::uint64_t active_revision{};
    std::optional<Identity> desired;
    std::optional<Identity> active_identity;
    std::optional<Identity> prepared_identity;
    std::optional<Request> request;
    std::optional<std::uint64_t> working_generation;
    std::optional<std::uint64_t> ready_generation;
    std::optional<std::uint64_t> prepared_generation;
    std::shared_ptr<IMySqlSession> active;
    std::shared_ptr<IMySqlSession> ready;
    std::vector<std::shared_ptr<IMySqlSession>> retired;
    bool active_healthy{true};
    std::optional<Error> failure;
    Clock::time_point retry_at{};
    Clock::time_point health_at{};
    Clock::time_point ready_expires_at{};
    std::optional<LogVolumeWindow> volume_window;
    std::uint64_t volume_revision{};
    LogVolumeReading volume_result;
    Clock::time_point volume_at{};
};

MySqlConfiguration::MySqlConfiguration(IMySqlCredentialsReader& credentials,
    IMySqlConnectionFactory& factory, std::chrono::milliseconds retry_interval,
    std::chrono::milliseconds health_interval, std::chrono::milliseconds query_interval)
    : state_(std::make_unique<State>(credentials, factory, retry_interval, health_interval, query_interval)) {}

MySqlConfiguration::~MySqlConfiguration() = default;

std::expected<bool, Error> MySqlConfiguration::prepare(const MySqlSettings& settings) {
    return state_->prepare(settings);
}

bool MySqlConfiguration::commit() noexcept {
    return state_->commit();
}

LogVolumeReading MySqlConfiguration::read_log_volume(const LogVolumeWindow& window) {
    return state_->read_log_volume(window);
}

}
