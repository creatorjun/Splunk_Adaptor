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

Error query_error() {
    return {"collect_count", "CloudESM.SUM_COLLECT_INFO_1DAY", "query_failure",
        "Database collection count query failed"};
}

Error silence_query_error() {
    return {"log_silence", "CloudESM.SUM_COLLECT_INFO", "query_failure",
        "Database last log receipt query failed"};
}

}

class MySqlConfiguration::State {
public:
    State(IMySqlCredentialsReader& reader, IMySqlConnectionFactory& connector,
        std::chrono::milliseconds retry, std::chrono::milliseconds health)
        : credentials(reader), factory(connector), retry_interval(retry), health_interval(health) {
        if (retry.count() <= 0 || health.count() <= 0) {
            throw std::invalid_argument("Database retry and health intervals must be positive");
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
        ++query_revision;
        if (!same_identity || !query_result.error || !active_identity->settings.enabled) {
            query_result = {};
            query_completed_at.reset();
            query_at = Clock::time_point{};
        }
        ++silence_revision;
        if (!same_identity || !silence_result.error || !active_identity->settings.enabled) {
            silence_result = {};
            silence_completed_at.reset();
            silence_at = Clock::time_point{};
        }
        ++volume_revision;
        if (!same_identity || !active_identity->settings.enabled) {
            volume_result = {};
            volume_at = Clock::time_point{};
        }
        if (!active_identity->settings.enabled) {
            desired_window.reset();
            silence_enabled = false;
            volume_window.reset();
        }
        failure.reset();
        retry_delay = std::min(retry_interval, maximum_retry_interval);
        health_at = Clock::now() + health_interval;
        wake.notify_one();
        return true;
    }

    CollectCountReading read_daily_counts(const DailyCountWindow& window,
        std::chrono::seconds refresh_interval) {
        if (refresh_interval.count() <= 0) {
            CollectCountReading result;
            result.status = CollectCountReadStatus::error;
            result.error = Error{"collect_count", "refresh_interval", "invalid_interval",
                "Collection count refresh interval must be positive"};
            return result;
        }
        std::lock_guard lock(mutex);
        if ((!active_identity || !active_identity->settings.enabled)
            && (!desired || !desired->settings.enabled) && !failure) {
            return {};
        }
        if (!desired_window || *desired_window != window) {
            desired_window = window;
            ++query_revision;
            query_result = {};
            query_completed_at.reset();
            query_at = Clock::time_point{};
        }
        if (query_interval != refresh_interval) {
            query_interval = refresh_interval;
            if (query_completed_at) {
                query_at = *query_completed_at + (query_result.status == CollectCountReadStatus::error
                    ? std::max(std::chrono::duration_cast<Clock::duration>(query_interval),
                        std::chrono::duration_cast<Clock::duration>(retry_interval))
                    : std::chrono::duration_cast<Clock::duration>(query_interval));
            }
        }
        wake.notify_one();
        if (!active || !active_healthy) {
            CollectCountReading result;
            result.queried_at = query_result.queried_at;
            if (query_result.error || failure || (active && !active_healthy)) {
                result.status = CollectCountReadStatus::error;
                result.error = query_result.error ? query_result.error : failure;
                if (!result.error) {
                    result.error = Error{"collect_count", "connection", "connection_unavailable",
                        "Database collection count connection is unavailable"};
                }
            }
            return result;
        }
        const auto maximum_age = std::max(query_interval * 2, std::chrono::seconds{10});
        if (query_result.status == CollectCountReadStatus::ready && query_completed_at
            && Clock::now() - *query_completed_at > maximum_age) {
            CollectCountReading result;
            result.queried_at = query_result.queried_at;
            return result;
        }
        return query_result;
    }

    LogSilenceReading read_last_received(bool enabled, std::chrono::seconds refresh_interval) {
        std::lock_guard lock(mutex);
        if (!enabled) {
            if (silence_enabled) {
                silence_enabled = false;
                ++silence_revision;
                silence_result = {};
                silence_completed_at.reset();
                silence_at = Clock::time_point{};
                wake.notify_one();
            }
            return {};
        }
        if (refresh_interval.count() <= 0) {
            LogSilenceReading result;
            result.status = LogSilenceReadStatus::error;
            result.error = Error{"log_silence", "refresh_interval", "invalid_interval",
                "Last log receipt refresh interval must be positive"};
            return result;
        }
        if ((!active_identity || !active_identity->settings.enabled)
            && (!desired || !desired->settings.enabled) && !failure) {
            return {};
        }
        if (!silence_enabled) {
            silence_enabled = true;
            ++silence_revision;
            silence_result = {};
            silence_completed_at.reset();
            silence_at = Clock::time_point{};
        }
        if (silence_interval != refresh_interval) {
            silence_interval = refresh_interval;
            if (silence_completed_at) {
                silence_at = *silence_completed_at + (silence_result.status == LogSilenceReadStatus::error
                    ? std::max(std::chrono::duration_cast<Clock::duration>(silence_interval),
                        std::chrono::duration_cast<Clock::duration>(retry_interval))
                    : std::chrono::duration_cast<Clock::duration>(silence_interval));
            }
        }
        wake.notify_one();
        if (!active || !active_healthy) {
            LogSilenceReading result;
            result.queried_at = silence_result.queried_at;
            if (silence_result.error || failure || (active && !active_healthy)) {
                result.status = LogSilenceReadStatus::error;
                result.error = silence_result.error ? silence_result.error : failure;
                if (!result.error) {
                    result.error = Error{"log_silence", "connection", "connection_unavailable",
                        "Database last log receipt connection is unavailable"};
                }
            }
            return result;
        }
        const auto maximum_age = std::max(silence_interval * 2, std::chrono::seconds{10});
        if (silence_result.status == LogSilenceReadStatus::ready && silence_completed_at
            && Clock::now() - *silence_completed_at > maximum_age) {
            LogSilenceReading result;
            result.queried_at = silence_result.queried_at;
            return result;
        }
        return silence_result;
    }

    LogVolumeReading read_log_volume(const LogVolumeWindow& window) {
        std::lock_guard lock(mutex);
        if (!volume_window || *volume_window != window) {
            volume_window = window;
            ++volume_revision;
            volume_result = {};
            volume_result.window = window;
            volume_at = Clock::time_point{};
        }
        wake.notify_one();
        if (volume_result.status == LogVolumeReadStatus::ready) return volume_result;
        if (!active || !active_healthy) {
            LogVolumeReading result;
            result.window = window;
            result.queried_at = volume_result.queried_at;
            if (volume_result.error || failure || (active && !active_healthy)) {
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
            if (active && active_healthy && Clock::now() >= health_at) {
                auto session = active;
                const auto revision = active_revision;
                lock.unlock();
                auto result = probe(*session);
                lock.lock();
                if (revision == active_revision) {
                    health_at = Clock::now() + health_interval;
                    if (!result) {
                        const auto original_failure = silence_result.error ? *silence_result.error
                            : (query_result.error ? *query_result.error : result.error());
                        active_healthy = false;
                        query_result.status = CollectCountReadStatus::error;
                        query_result.counts.clear();
                        if (!query_result.error) {
                            query_result.error = result.error();
                            query_at = Clock::time_point{};
                        }
                        silence_result.status = LogSilenceReadStatus::error;
                        silence_result.batch.reset();
                        if (!silence_result.error) {
                            silence_result.error = result.error();
                            silence_at = Clock::time_point{};
                        }
                        if (active_identity && desired && *active_identity == *desired) {
                            if (!failure) failure = original_failure;
                            retry_at = Clock::now();
                        }
                    }
                }
                lock.unlock();
                session.reset();
                lock.lock();
                continue;
            }
            if (active && active_healthy && desired_window && Clock::now() >= query_at) {
                auto session = active;
                const auto revision = active_revision;
                const auto selected_query_revision = query_revision;
                const auto window = *desired_window;
                lock.unlock();
                auto result = query(*session, window);
                const auto queried_at = std::chrono::system_clock::now();
                lock.lock();
                if (!stopping && revision == active_revision
                    && selected_query_revision == query_revision && desired_window
                    && *desired_window == window) {
                    query_completed_at = Clock::now();
                    if (result) {
                        query_result = {};
                        query_result.queried_at = queried_at;
                        query_result.status = CollectCountReadStatus::ready;
                        query_result.counts = std::move(*result);
                        query_at = *query_completed_at + query_interval;
                    } else {
                        query_result.status = CollectCountReadStatus::error;
                        query_result.counts.clear();
                        if (!query_result.error) {
                            query_result.error = std::move(result.error());
                            query_result.queried_at = queried_at;
                        }
                        query_at = *query_completed_at + std::max(
                            std::chrono::duration_cast<Clock::duration>(query_interval),
                            std::chrono::duration_cast<Clock::duration>(retry_interval));
                        health_at = Clock::now();
                    }
                }
                lock.unlock();
                session.reset();
                lock.lock();
                continue;
            }
            if (active && active_healthy && silence_enabled && Clock::now() >= silence_at) {
                auto session = active;
                const auto revision = active_revision;
                const auto selected_silence_revision = silence_revision;
                lock.unlock();
                auto result = query_last_received(*session);
                const auto queried_at = std::chrono::system_clock::now();
                lock.lock();
                if (!stopping && revision == active_revision
                    && selected_silence_revision == silence_revision && silence_enabled) {
                    silence_completed_at = Clock::now();
                    if (result) {
                        silence_result = {};
                        silence_result.queried_at = queried_at;
                        silence_result.status = LogSilenceReadStatus::ready;
                        silence_result.batch = std::move(*result);
                        silence_at = *silence_completed_at + silence_interval;
                    } else {
                        silence_result.status = LogSilenceReadStatus::error;
                        silence_result.batch.reset();
                        if (!silence_result.error) {
                            silence_result.error = std::move(result.error());
                            silence_result.queried_at = queried_at;
                        }
                        silence_at = *silence_completed_at + std::max(
                            std::chrono::duration_cast<Clock::duration>(silence_interval),
                            std::chrono::duration_cast<Clock::duration>(retry_interval));
                        health_at = Clock::now();
                    }
                }
                lock.unlock();
                session.reset();
                lock.lock();
                continue;
            }
            if (active && active_healthy && volume_window && Clock::now() >= volume_at) {
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
                    } else {
                        volume_result.status = LogVolumeReadStatus::error;
                        volume_result.equipment.clear();
                        if (!volume_result.error) {
                            volume_result.error = std::move(result.error());
                            volume_result.queried_at = queried_at;
                        }
                        volume_at = Clock::now() + retry_interval;
                        health_at = Clock::now();
                    }
                }
                lock.unlock();
                session.reset();
                lock.lock();
                continue;
            }
            if (active && active_healthy) {
                auto deadline = health_at;
                if (desired_window) deadline = std::min(deadline, query_at);
                if (silence_enabled) deadline = std::min(deadline, silence_at);
                if (volume_window) deadline = std::min(deadline, volume_at);
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

    static std::expected<std::vector<DailyCountPair>, Error> query(
        IMySqlSession& session, const DailyCountWindow& window) {
        try {
            return session.read_daily_counts(window);
        } catch (...) {
            return std::unexpected(query_error());
        }
    }

    static std::expected<LogReceiptBatch, Error> query_last_received(IMySqlSession& session) {
        try {
            return session.read_last_received();
        } catch (...) {
            return std::unexpected(silence_query_error());
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
    std::optional<DailyCountWindow> desired_window;
    std::chrono::seconds query_interval{60};
    std::uint64_t query_revision{};
    CollectCountReading query_result;
    std::optional<Clock::time_point> query_completed_at;
    Clock::time_point query_at{};
    bool silence_enabled{};
    std::chrono::seconds silence_interval{60};
    std::uint64_t silence_revision{};
    LogSilenceReading silence_result;
    std::optional<Clock::time_point> silence_completed_at;
    Clock::time_point silence_at{};
    std::optional<LogVolumeWindow> volume_window;
    std::uint64_t volume_revision{};
    LogVolumeReading volume_result;
    Clock::time_point volume_at{};
};

MySqlConfiguration::MySqlConfiguration(IMySqlCredentialsReader& credentials,
    IMySqlConnectionFactory& factory, std::chrono::milliseconds retry_interval,
    std::chrono::milliseconds health_interval)
    : state_(std::make_unique<State>(credentials, factory, retry_interval, health_interval)) {}

MySqlConfiguration::~MySqlConfiguration() = default;

std::expected<bool, Error> MySqlConfiguration::prepare(const MySqlSettings& settings) {
    return state_->prepare(settings);
}

bool MySqlConfiguration::commit() noexcept {
    return state_->commit();
}

CollectCountReading MySqlConfiguration::read_daily_counts(const DailyCountWindow& window,
    std::chrono::seconds refresh_interval) {
    return state_->read_daily_counts(window, refresh_interval);
}

LogSilenceReading MySqlConfiguration::read_last_received(bool enabled,
    std::chrono::seconds refresh_interval) {
    return state_->read_last_received(enabled, refresh_interval);
}

LogVolumeReading MySqlConfiguration::read_log_volume(const LogVolumeWindow& window) {
    return state_->read_log_volume(window);
}

}
