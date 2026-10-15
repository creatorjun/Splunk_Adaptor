// src/infrastructure/database/MySqlConfiguration.cpp
#include "infrastructure/database/MySqlConfiguration.hpp"

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
        std::chrono::milliseconds retry, std::chrono::milliseconds health)
        : credentials(reader), factory(connector), retry_interval(retry), health_interval(health) {
        if (retry.count() <= 0 || health.count() <= 0) {
            throw std::invalid_argument("Database retry and health intervals must be positive");
        }
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
            failure.reset();
            wake.notify_one();
        }
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
        failure.reset();
        health_at = Clock::now() + health_interval;
        wake.notify_one();
        return true;
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
                auto connected = connect(task.identity);
                lock.lock();
                working_generation.reset();
                if (!stopping && generation == task.generation && desired && *desired == task.identity) {
                    if (connected) {
                        ready = std::move(*connected);
                        ready_generation = task.generation;
                        ready_expires_at = Clock::now() + health_interval;
                        failure.reset();
                    } else {
                        failure = connected.error();
                        retry_at = Clock::now() + retry_interval;
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
                        active_healthy = false;
                        if (active_identity && desired && *active_identity == *desired) {
                            failure = result.error();
                            retry_at = Clock::now();
                        }
                    }
                }
                lock.unlock();
                session.reset();
                lock.lock();
                continue;
            }
            if (active && active_healthy) {
                wake.wait_until(lock, health_at);
            } else {
                wake.wait(lock);
            }
        }
        auto discarded_active = std::move(active);
        auto discarded_ready = std::move(ready);
        auto discarded_retired = std::move(retired);
        lock.unlock();
    }

    std::expected<std::shared_ptr<IMySqlSession>, Error> connect(const Identity& identity) {
        try {
            auto result = factory.connect(identity.settings, identity.credentials);
            if (!result) {
                return std::unexpected(result.error());
            }
            if (!*result) {
                return std::unexpected(worker_error());
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

    IMySqlCredentialsReader& credentials;
    IMySqlConnectionFactory& factory;
    std::chrono::milliseconds retry_interval;
    std::chrono::milliseconds health_interval;
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

}
