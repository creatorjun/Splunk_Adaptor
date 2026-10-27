// tests/MySqlConfigurationTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/database/MySqlConfiguration.hpp"

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <map>
#include <mutex>
#include <pthread.h>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace monitor;
using namespace std::chrono_literals;

struct Reader final : IMySqlCredentialsReader {
    std::expected<MySqlCredentials, Error> next = MySqlCredentials{"monitor", "first-password"};
    unsigned int reads{};

    std::expected<MySqlCredentials, Error> read_credentials(const std::string&) override {
        ++reads;
        return next;
    }
};

struct Attempt {
    MySqlSettings settings;
    MySqlCredentials credentials;
    std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
};

struct QueryAttempt {
    std::size_t session;
    DailyCountWindow window;
};

struct Control {
    std::mutex mutex;
    std::condition_variable wake;
    bool blocked{};
    bool fail_connect{};
    bool throw_connect{};
    bool signals_masked{};
    bool query_blocked{};
    bool fail_query{};
    bool throw_query{};
    bool silence_blocked{};
    bool fail_silence{};
    bool throw_silence{};
    std::string silence_error_code{"query_failed"};
    std::chrono::system_clock::time_point last_receipt{std::chrono::seconds{1792380600}};
    std::uint64_t current_count{110};
    std::vector<Attempt> attempts;
    std::vector<QueryAttempt> queries;
    std::vector<std::size_t> silence_queries;
    std::vector<std::chrono::steady_clock::time_point> silence_query_times;
    std::set<std::size_t> live;
    std::set<std::size_t> closed;
    std::set<std::size_t> fail_probe;
    std::map<std::size_t, unsigned int> probes;
};

struct Session final : IMySqlSession {
    Session(Control& control, std::size_t identity) : state(control), id(identity) {
        std::lock_guard lock(state.mutex);
        state.live.insert(id);
    }

    ~Session() override {
        std::lock_guard lock(state.mutex);
        state.live.erase(id);
        state.closed.insert(id);
    }

    std::expected<void, Error> probe() override {
        std::lock_guard lock(state.mutex);
        ++state.probes[id];
        if (state.fail_probe.contains(id)) {
            return std::unexpected(Error{"mysql", "connection", "2006", "Database health check failed"});
        }
        return {};
    }

    std::expected<std::vector<DailyCountPair>, Error> read_daily_counts(
        const DailyCountWindow& window) override {
        std::unique_lock lock(state.mutex);
        state.queries.push_back({id, window});
        const auto current_count = state.current_count;
        state.wake.wait_for(lock, 15s, [&] { return !state.query_blocked; });
        if (state.throw_query) {
            throw std::runtime_error("sensitive query exception");
        }
        if (state.fail_query) {
            return std::unexpected(Error{"collect_count", "table", "query_failed", "Query failed"});
        }
        return std::vector<DailyCountPair>{{"192.0.2.1", current_count, 100, true, true}};
    }

    std::expected<LogReceiptBatch, Error> read_last_received() override {
        std::unique_lock lock(state.mutex);
        state.silence_queries.push_back(id);
        state.silence_query_times.push_back(std::chrono::steady_clock::now());
        const auto received = state.last_receipt;
        state.wake.wait_for(lock, 15s, [&] { return !state.silence_blocked; });
        if (state.throw_silence) throw std::runtime_error("sensitive last receipt exception");
        if (state.fail_silence) {
            return std::unexpected(Error{"log_silence", "CloudESM.SUM_COLLECT_INFO",
                state.silence_error_code, "Last log receipt query failed"});
        }
        return LogReceiptBatch{received + 300s, {{"192.0.2.1", received}}};
    }

    Control& state;
    std::size_t id;
};

struct Factory final : IMySqlConnectionFactory {
    std::expected<std::unique_ptr<IMySqlSession>, Error> connect(
        const MySqlSettings& settings, const MySqlCredentials& credentials) override {
        std::unique_lock lock(state.mutex);
        state.attempts.push_back({settings, credentials});
        const auto id = state.attempts.size();
        sigset_t signals;
        CHECK(::pthread_sigmask(SIG_BLOCK, nullptr, &signals) == 0);
        state.signals_masked = ::sigismember(&signals, SIGTERM) == 1
            && ::sigismember(&signals, SIGINT) == 1;
        state.wake.wait_for(lock, 2s, [&] { return !state.blocked; });
        if (state.throw_connect) {
            throw std::runtime_error("sensitive exception text");
        }
        if (state.fail_connect) {
            return std::unexpected(Error{"mysql", "connection", "1045", "Database connection failed"});
        }
        lock.unlock();
        return std::make_unique<Session>(state, id);
    }

    template <typename Function>
    auto inspect(Function function) {
        std::lock_guard lock(state.mutex);
        return function(state);
    }

    void release() {
        {
            std::lock_guard lock(state.mutex);
            state.blocked = false;
        }
        state.wake.notify_all();
    }

    void release_query() {
        {
            std::lock_guard lock(state.mutex);
            state.query_blocked = false;
        }
        state.wake.notify_all();
    }

    void release_silence() {
        {
            std::lock_guard lock(state.mutex);
            state.silence_blocked = false;
        }
        state.wake.notify_all();
    }

    Control state;
};

template <typename Predicate>
void wait_for(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

void wait_ready(MySqlConfiguration& configuration, const MySqlSettings& settings) {
    wait_for([&] {
        const auto result = configuration.prepare(settings);
        return result && *result;
    });
}

MySqlSettings enabled_settings() {
    MySqlSettings settings;
    settings.enabled = true;
    settings.database = "monitoring";
    return settings;
}

void pending_and_rotation_tests() {
    Reader reader;
    Factory factory;
    factory.state.blocked = true;
    MySqlConfiguration configuration(reader, factory, 50ms, 5s);
    auto settings = enabled_settings();
    const auto started = std::chrono::steady_clock::now();
    const auto pending = configuration.prepare(settings);
    CHECK(pending && !*pending);
    CHECK(std::chrono::steady_clock::now() - started < 100ms);
    CHECK(!configuration.commit());
    wait_for([&] { return factory.inspect([](const Control& state) { return state.attempts.size() == 1; }); });
    CHECK(factory.inspect([](const Control& state) { return state.signals_masked; }));
    factory.release();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    CHECK(!configuration.commit());
    for (int index = 0; index < 10; ++index) {
        CHECK(configuration.prepare(settings).value());
        CHECK(!configuration.commit());
    }
    CHECK(factory.inspect([](const Control& state) { return state.attempts.size() == 1; }));
    reader.next = MySqlCredentials{"monitor", "rotated-password"};
    CHECK(!configuration.prepare(settings).value());
    wait_ready(configuration, settings);
    CHECK(factory.inspect([](const Control& state) { return state.live == std::set<std::size_t>{1, 2}; }));
    CHECK(configuration.commit());
    wait_for([&] { return factory.inspect([](const Control& state) { return state.live == std::set<std::size_t>{2}; }); });
    CHECK(factory.inspect([](const Control& state) { return state.attempts.back().credentials.password == "rotated-password"; }));
    const auto reads = reader.reads;
    settings.enabled = false;
    CHECK(configuration.prepare(settings).value());
    CHECK(reader.reads == reads);
    CHECK(configuration.commit());
    wait_for([&] { return factory.inspect([](const Control& state) { return state.live.empty(); }); });
}

void failure_and_retry_tests() {
    Reader reader;
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 150ms, 5s);
    auto settings = enabled_settings();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    settings.host = "new-host";
    factory.inspect([](Control& state) { state.fail_connect = true; });
    CHECK(!configuration.prepare(settings).value());
    wait_for([&] { return !configuration.prepare(settings); });
    CHECK(!configuration.commit());
    CHECK(factory.inspect([](const Control& state) { return state.live == std::set<std::size_t>{1}; }));
    for (int index = 0; index < 10; ++index) {
        CHECK(!configuration.prepare(settings));
    }
    CHECK(factory.inspect([](const Control& state) { return state.attempts.size() == 2; }));
    factory.inspect([](Control& state) { state.fail_connect = false; });
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    wait_for([&] { return factory.inspect([](const Control& state) { return state.live == std::set<std::size_t>{3}; }); });
    reader.next = std::unexpected(Error{"mysql_credentials", "credentials", "invalid_json", "Invalid credentials"});
    CHECK(!configuration.prepare(settings));
    CHECK(!configuration.commit());
    CHECK(factory.inspect([](const Control& state) { return state.live == std::set<std::size_t>{3}; }));
}

void stale_candidate_tests() {
    Reader reader;
    Factory factory;
    factory.state.blocked = true;
    MySqlConfiguration configuration(reader, factory, 50ms, 5s);
    auto settings = enabled_settings();
    CHECK(!configuration.prepare(settings).value());
    wait_for([&] { return factory.inspect([](const Control& state) { return state.attempts.size() == 1; }); });
    settings.port = 4406;
    reader.next = MySqlCredentials{"new-user", "new-password"};
    CHECK(!configuration.prepare(settings).value());
    CHECK(!configuration.commit());
    factory.release();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    wait_for([&] { return factory.inspect([](const Control& state) { return state.live == std::set<std::size_t>{2}; }); });
    CHECK(factory.inspect([](const Control& state) { return state.attempts.back().settings.port == 4406
        && state.attempts.back().credentials.username == "new-user"; }));
    CHECK(factory.inspect([](const Control& state) { return !state.probes.contains(1); }));
    settings.database = "replacement";
    wait_ready(configuration, settings);
    reader.next = std::unexpected(Error{"mysql_credentials", "credentials", "unreadable", "Unreadable credentials"});
    CHECK(!configuration.prepare(settings));
    CHECK(!configuration.commit());
    wait_for([&] { return factory.inspect([](const Control& state) { return state.live == std::set<std::size_t>{2}; }); });
}

void health_recovery_tests() {
    Reader reader;
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 20ms, 20ms);
    const auto settings = enabled_settings();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    factory.inspect([](Control& state) { state.fail_probe.insert(1); });
    wait_for([&] { return factory.inspect([](const Control& state) { return state.probes.at(1) >= 2; }); });
    wait_for([&] {
        auto prepared = configuration.prepare(settings);
        return prepared && *prepared && configuration.commit();
    });
    wait_for([&] { return factory.inspect([](const Control& state) { return state.live == std::set<std::size_t>{2}; }); });
}

void failed_probe_and_cancel_tests() {
    Reader reader;
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 5s, 5s);
    auto settings = enabled_settings();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    factory.inspect([](Control& state) { state.fail_probe.insert(2); });
    settings.host = "bad-probe-host";
    wait_for([&] { return !configuration.prepare(settings); });
    CHECK(!configuration.commit());
    CHECK(factory.inspect([](const Control& state) { return state.live == std::set<std::size_t>{1}; }));
    factory.inspect([](Control& state) { state.blocked = true; });
    settings.port = 5506;
    CHECK(!configuration.prepare(settings).value());
    wait_for([&] { return factory.inspect([](const Control& state) { return state.attempts.size() == 3; }); });
    settings.enabled = false;
    CHECK(configuration.prepare(settings).value());
    CHECK(configuration.commit());
    const auto reads = reader.reads;
    factory.release();
    wait_for([&] { return factory.inspect([](const Control& state) { return state.closed.contains(3); }); });
    CHECK(factory.inspect([](const Control& state) { return state.live.empty() && !state.probes.contains(3); }));
    CHECK(configuration.prepare(settings).value());
    CHECK(!configuration.commit());
    CHECK(reader.reads == reads);
    CHECK(factory.inspect([](const Control& state) { return state.attempts.size() == 3; }));
}

void disabled_health_and_retry_tests() {
    Reader reader;
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 20ms, 20ms);
    auto settings = enabled_settings();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    wait_for([&] { return factory.inspect([](const Control& state) { return state.probes.at(1) >= 2; }); });
    settings.enabled = false;
    CHECK(configuration.prepare(settings).value());
    CHECK(configuration.commit());
    wait_for([&] { return factory.inspect([](const Control& state) { return state.closed.contains(1); }); });
    const auto probes = factory.inspect([](const Control& state) { return state.probes; });
    const auto reads = reader.reads;
    reader.next = std::unexpected(Error{"mysql_credentials", "credentials", "unreadable", "Unreadable credentials"});
    std::this_thread::sleep_for(60ms);
    for (int index = 0; index < 10; ++index) {
        CHECK(configuration.prepare(settings).value());
        CHECK(!configuration.commit());
    }
    CHECK(reader.reads == reads);
    CHECK(factory.inspect([&](const Control& state) {
        return state.attempts.size() == 1 && state.live.empty() && state.probes == probes;
    }));
}

void expired_candidate_tests() {
    Reader reader;
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 50ms, 30ms);
    auto settings = enabled_settings();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    settings.host = "replacement-host";
    wait_ready(configuration, settings);
    CHECK(factory.inspect([](const Control& state) { return state.live == std::set<std::size_t>{1, 2}; }));
    factory.inspect([](Control& state) { state.blocked = true; });
    std::this_thread::sleep_for(70ms);
    const auto prepared = configuration.prepare(settings);
    CHECK(prepared && !*prepared);
    CHECK(!configuration.commit());
    wait_for([&] { return factory.inspect([](const Control& state) { return state.attempts.size() == 3; }); });
    CHECK(factory.inspect([](const Control& state) { return state.live == std::set<std::size_t>{1}; }));
    factory.release();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    wait_for([&] { return factory.inspect([](const Control& state) { return state.live == std::set<std::size_t>{3}; }); });
}

void exception_redaction_tests() {
    Reader reader;
    Factory factory;
    factory.state.throw_connect = true;
    MySqlConfiguration configuration(reader, factory, 5s, 5s);
    const auto settings = enabled_settings();
    wait_for([&] { return !configuration.prepare(settings); });
    const auto result = configuration.prepare(settings);
    CHECK(!result);
    CHECK(result.error().code == "worker_failure");
    CHECK(result.error().message.find("sensitive") == std::string::npos);
}

void query_initialization_and_refresh_tests() {
    Reader reader;
    Factory factory;
    factory.state.blocked = true;
    MySqlConfiguration configuration(reader, factory, 20ms, 5s);
    const auto settings = enabled_settings();
    const DailyCountWindow window{20261018, 20261017};
    CHECK(configuration.read_daily_counts(window, 1s).status == CollectCountReadStatus::pending);
    CHECK(factory.inspect([](const Control& state) { return state.queries.empty(); }));
    CHECK(!configuration.prepare(settings).value());
    CHECK(configuration.read_daily_counts(window, 1s).status == CollectCountReadStatus::pending);
    factory.release();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    wait_for([&] {
        return configuration.read_daily_counts(window, 1s).status == CollectCountReadStatus::ready;
    });
    const auto first = configuration.read_daily_counts(window, 1s);
    CHECK(first.queried_at.has_value());
    CHECK(first.counts.size() == 1 && first.counts.front().current_count == 110);
    factory.inspect([](Control& state) { state.current_count = 125; });
    wait_for([&] {
        const auto current = configuration.read_daily_counts(window, 1s);
        return current.status == CollectCountReadStatus::ready
            && current.counts.front().current_count == 125;
    });
    CHECK(factory.inspect([](const Control& state) {
        return state.attempts.size() == 1 && state.queries.size() >= 2;
    }));
    CHECK(configuration.read_daily_counts(window, 0s).status == CollectCountReadStatus::error);
}

void query_disable_and_window_change_tests() {
    Reader reader;
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 20ms, 5s);
    auto settings = enabled_settings();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    const DailyCountWindow first_window{20261018, 20261017};
    const DailyCountWindow second_window{20261019, 20261018};
    factory.inspect([](Control& state) { state.query_blocked = true; });
    CHECK(configuration.read_daily_counts(first_window, 1s).status == CollectCountReadStatus::pending);
    wait_for([&] { return factory.inspect([](const Control& state) { return state.queries.size() == 1; }); });
    factory.inspect([](Control& state) { state.current_count = 140; });
    CHECK(configuration.read_daily_counts(second_window, 1s).status == CollectCountReadStatus::pending);
    factory.release_query();
    wait_for([&] {
        const auto result = configuration.read_daily_counts(second_window, 1s);
        return result.status == CollectCountReadStatus::ready
            && result.counts.front().current_count == 140;
    });
    CHECK(factory.inspect([&](const Control& state) {
        return state.queries.size() == 2 && state.queries.back().window == second_window;
    }));
    factory.inspect([](Control& state) { state.query_blocked = true; });
    CHECK(configuration.read_daily_counts(first_window, 1s).status == CollectCountReadStatus::pending);
    wait_for([&] { return factory.inspect([](const Control& state) { return state.queries.size() == 3; }); });
    const auto started = std::chrono::steady_clock::now();
    settings.enabled = false;
    CHECK(configuration.prepare(settings).value());
    CHECK(configuration.commit());
    CHECK(std::chrono::steady_clock::now() - started < 100ms);
    CHECK(configuration.read_daily_counts(first_window, 1s).status == CollectCountReadStatus::pending);
    factory.release_query();
    wait_for([&] { return factory.inspect([](const Control& state) { return state.live.empty(); }); });
    CHECK(configuration.read_daily_counts(first_window, 1s).counts.empty());
    std::this_thread::sleep_for(30ms);
    CHECK(factory.inspect([](const Control& state) { return state.queries.size() == 3; }));
}

void query_connection_rotation_tests() {
    Reader reader;
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 20ms, 5s);
    auto settings = enabled_settings();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    settings.host = "new-query-host";
    wait_ready(configuration, settings);
    const DailyCountWindow window{20261018, 20261017};
    factory.inspect([](Control& state) { state.query_blocked = true; });
    CHECK(configuration.read_daily_counts(window, 1s).status == CollectCountReadStatus::pending);
    wait_for([&] { return factory.inspect([](const Control& state) { return state.queries.size() == 1; }); });
    CHECK(configuration.commit());
    CHECK(configuration.read_daily_counts(window, 1s).status == CollectCountReadStatus::pending);
    factory.inspect([](Control& state) { state.current_count = 160; });
    factory.release_query();
    wait_for([&] {
        const auto result = configuration.read_daily_counts(window, 1s);
        return result.status == CollectCountReadStatus::ready
            && result.counts.front().current_count == 160;
    });
    CHECK(factory.inspect([](const Control& state) {
        return state.queries.size() == 2 && state.queries[0].session == 1
            && state.queries[1].session == 2;
    }));
}

void query_error_and_recovery_tests() {
    Reader reader;
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 1200ms, 5s);
    const auto settings = enabled_settings();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    const DailyCountWindow window{20261018, 20261017};
    factory.inspect([](Control& state) { state.throw_query = true; });
    wait_for([&] {
        return configuration.read_daily_counts(window, 1s).status == CollectCountReadStatus::error;
    });
    const auto failed = configuration.read_daily_counts(window, 1s);
    CHECK(failed.counts.empty() && failed.error.has_value());
    CHECK(failed.error->message.find("sensitive") == std::string::npos);
    std::this_thread::sleep_for(100ms);
    CHECK(factory.inspect([](const Control& state) { return state.queries.size() == 1; }));
    factory.inspect([](Control& state) { state.throw_query = false; });
    wait_for([&] {
        return configuration.read_daily_counts(window, 1s).status == CollectCountReadStatus::ready;
    });
    CHECK(!configuration.read_daily_counts(window, 1s).error.has_value());
    CHECK(factory.inspect([](const Control& state) { return state.attempts.size() == 1; }));
}

void query_stale_cache_tests() {
    Reader reader;
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 20ms, 20s);
    const auto settings = enabled_settings();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    const DailyCountWindow window{20261018, 20261017};
    wait_for([&] {
        return configuration.read_daily_counts(window, 1s).status == CollectCountReadStatus::ready;
    });
    factory.inspect([](Control& state) { state.query_blocked = true; });
    wait_for([&] { return factory.inspect([](const Control& state) { return state.queries.size() == 2; }); });
    std::this_thread::sleep_for(10s);
    const auto started = std::chrono::steady_clock::now();
    const auto stale = configuration.read_daily_counts(window, 1s);
    CHECK(std::chrono::steady_clock::now() - started < 100ms);
    CHECK(stale.status == CollectCountReadStatus::pending && stale.counts.empty());
    CHECK(stale.queried_at.has_value());
    factory.release_query();
    wait_for([&] {
        return configuration.read_daily_counts(window, 1s).status == CollectCountReadStatus::ready;
    });
}

void query_interval_reload_tests() {
    Reader reader;
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 20ms, 20s);
    const auto settings = enabled_settings();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    const DailyCountWindow window{20261018, 20261017};
    wait_for([&] {
        return configuration.read_daily_counts(window, 1s).status == CollectCountReadStatus::ready;
    });
    CHECK(configuration.read_daily_counts(window, 86400s).status == CollectCountReadStatus::ready);
    std::this_thread::sleep_for(1200ms);
    CHECK(factory.inspect([](const Control& state) { return state.queries.size() == 1; }));
    configuration.read_daily_counts(window, 1s);
    wait_for([&] { return factory.inspect([](const Control& state) { return state.queries.size() == 2; }); });
    CHECK(factory.inspect([](const Control& state) { return state.attempts.size() == 1; }));
}

void silence_refresh_and_cancel_tests() {
    Reader reader;
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 20ms, 20s);
    auto settings = enabled_settings();
    CHECK(configuration.read_last_received(true, 1s).status == LogSilenceReadStatus::pending);
    CHECK(factory.inspect([](const Control& state) { return state.silence_queries.empty(); }));
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    wait_for([&] { return configuration.read_last_received(true, 1s).status == LogSilenceReadStatus::ready; });
    const auto initial = configuration.read_last_received(true, 86400s);
    CHECK(initial.batch && initial.queried_at);
    CHECK(initial.batch->records.size() == 1);
    std::this_thread::sleep_for(1100ms);
    CHECK(factory.inspect([](const Control& state) { return state.silence_queries.size() == 1; }));
    factory.inspect([](Control& state) { state.last_receipt += 60s; });
    configuration.read_last_received(true, 1s);
    wait_for([&] {
        const auto current = configuration.read_last_received(true, 1s);
        return current.batch && current.batch->observed_at == initial.batch->observed_at + 60s;
    });
    CHECK(factory.inspect([](const Control& state) { return state.attempts.size() == 1; }));
    CHECK(configuration.read_last_received(true, 0s).status == LogSilenceReadStatus::error);
    CHECK(configuration.read_last_received(false, 0s).status == LogSilenceReadStatus::pending);
    factory.inspect([](Control& state) { state.silence_blocked = true; });
    configuration.read_last_received(true, 1s);
    wait_for([&] { return factory.inspect([](const Control& state) { return state.silence_queries.size() == 3; }); });
    const auto started = std::chrono::steady_clock::now();
    CHECK(!configuration.read_last_received(false, 1s).batch);
    CHECK(std::chrono::steady_clock::now() - started < 100ms);
    factory.release_silence();
    std::this_thread::sleep_for(1100ms);
    CHECK(factory.inspect([](const Control& state) { return state.silence_queries.size() == 3; }));
    CHECK(!configuration.read_last_received(false, 1s).batch);
    factory.inspect([](Control& state) { state.last_receipt += 60s; });
    wait_for([&] {
        const auto current = configuration.read_last_received(true, 1s);
        return current.batch && current.batch->observed_at == initial.batch->observed_at + 120s;
    });
    settings.enabled = false;
    CHECK(configuration.prepare(settings).value());
    CHECK(configuration.commit());
    CHECK(!configuration.read_last_received(true, 1s).batch);
    std::this_thread::sleep_for(1100ms);
    CHECK(factory.inspect([](const Control& state) { return state.silence_queries.size() == 4; }));
}

void silence_rotation_tests() {
    Reader reader;
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 20ms, 20s);
    auto settings = enabled_settings();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    settings.host = "new-last-receipt-host";
    wait_ready(configuration, settings);
    factory.inspect([](Control& state) { state.silence_blocked = true; });
    configuration.read_last_received(true, 1s);
    wait_for([&] { return factory.inspect([](const Control& state) { return state.silence_queries.size() == 1; }); });
    CHECK(configuration.commit());
    CHECK(configuration.read_last_received(true, 1s).status == LogSilenceReadStatus::pending);
    const auto expected = factory.inspect([](Control& state) { state.last_receipt += 60s; return state.last_receipt; });
    factory.release_silence();
    wait_for([&] {
        const auto current = configuration.read_last_received(true, 1s);
        return current.batch && current.batch->records.front().last_received_at == expected;
    });
    CHECK(factory.inspect([](const Control& state) { return state.silence_queries == std::vector<std::size_t>{1, 2}; }));
}

void silence_error_isolation_tests() {
    Reader reader;
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 1100ms, 20s);
    const auto settings = enabled_settings();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    const DailyCountWindow window{20261018, 20261017};
    wait_for([&] { return configuration.read_daily_counts(window, 60s).status == CollectCountReadStatus::ready; });
    factory.inspect([](Control& state) { state.throw_silence = true; });
    wait_for([&] { return configuration.read_last_received(true, 1s).status == LogSilenceReadStatus::error; });
    const auto failed = configuration.read_last_received(true, 1s);
    CHECK(!failed.batch && failed.error && failed.queried_at);
    CHECK(failed.error->message.find("sensitive") == std::string::npos);
    CHECK(configuration.read_daily_counts(window, 60s).status == CollectCountReadStatus::ready);
    std::this_thread::sleep_for(100ms);
    CHECK(factory.inspect([](const Control& state) { return state.silence_queries.size() == 1; }));
    factory.inspect([](Control& state) { state.throw_silence = false; });
    wait_for([&] { return configuration.read_last_received(true, 1s).status == LogSilenceReadStatus::ready; });
    CHECK(!configuration.read_last_received(true, 1s).error);
    factory.inspect([](Control& state) { state.fail_query = true; });
    wait_for([&] { return configuration.read_daily_counts(window, 1s).status == CollectCountReadStatus::error; });
    CHECK(configuration.read_last_received(true, 1s).status == LogSilenceReadStatus::ready);
    CHECK(factory.inspect([](const Control& state) { return state.attempts.size() == 1; }));
}

void silence_stale_cache_tests() {
    Reader reader;
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 20ms, 20s);
    const auto settings = enabled_settings();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    wait_for([&] { return configuration.read_last_received(true, 1s).status == LogSilenceReadStatus::ready; });
    factory.inspect([](Control& state) { state.silence_blocked = true; });
    wait_for([&] { return factory.inspect([](const Control& state) { return state.silence_queries.size() == 2; }); });
    std::this_thread::sleep_for(10s);
    const auto started = std::chrono::steady_clock::now();
    const auto stale = configuration.read_last_received(true, 1s);
    CHECK(std::chrono::steady_clock::now() - started < 100ms);
    CHECK(stale.status == LogSilenceReadStatus::pending && !stale.batch && stale.queried_at);
    factory.release_silence();
    wait_for([&] { return configuration.read_last_received(true, 1s).status == LogSilenceReadStatus::ready; });
}

void silence_health_invalidation_tests() {
    Reader reader;
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 20ms, 40ms);
    const auto settings = enabled_settings();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    wait_for([&] { return configuration.read_last_received(true, 60s).status == LogSilenceReadStatus::ready; });
    factory.inspect([](Control& state) { state.fail_probe.insert(1); });
    wait_for([&] { return configuration.read_last_received(true, 60s).status == LogSilenceReadStatus::error; });
    CHECK(!configuration.read_last_received(true, 60s).batch);
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    wait_for([&] { return configuration.read_last_received(true, 60s).status == LogSilenceReadStatus::ready; });
    CHECK(factory.inspect([](const Control& state) { return state.silence_queries == std::vector<std::size_t>{1, 2}; }));
}

void connection_error_survives_retry_tests() {
    Reader reader;
    Factory factory;
    factory.state.fail_connect = true;
    MySqlConfiguration configuration(reader, factory, 150ms, 5s);
    const auto settings = enabled_settings();
    CHECK(!configuration.prepare(settings).value());
    wait_for([&] { return !configuration.prepare(settings); });
    const auto failed = configuration.read_last_received(true, 1s);
    CHECK(failed.status == LogSilenceReadStatus::error);
    CHECK(failed.error && failed.error->code == "1045" && !failed.batch);
    CHECK(!failed.queried_at);
    factory.inspect([](Control& state) { state.fail_connect = false; state.blocked = true; });
    wait_for([&] {
        const auto prepared = configuration.prepare(settings);
        CHECK(!prepared && prepared.error().code == "1045");
        return factory.inspect([](const Control& state) { return state.attempts.size() == 2; });
    });
    CHECK(configuration.read_last_received(true, 1s).error->code == "1045");
    const auto daily = configuration.read_daily_counts({20261018, 20261017}, 60s);
    CHECK(daily.status == CollectCountReadStatus::error && daily.error->code == "1045");
    factory.release();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    wait_for([&] {
        const auto result = configuration.read_last_received(true, 1s);
        return result.status == LogSilenceReadStatus::ready && !result.error;
    });
}

void reconnect_backoff_tests() {
    Reader reader;
    Factory factory;
    factory.state.fail_connect = true;
    MySqlConfiguration configuration(reader, factory, 50ms, 5s);
    auto settings = enabled_settings();
    wait_for([&] {
        configuration.prepare(settings);
        return factory.inspect([](const Control& state) { return state.attempts.size() >= 4; });
    });
    const auto attempts = factory.inspect([](const Control& state) { return state.attempts; });
    CHECK(attempts[1].started - attempts[0].started >= 50ms);
    CHECK(attempts[2].started - attempts[1].started >= 100ms);
    CHECK(attempts[3].started - attempts[2].started >= 200ms);
    factory.inspect([](Control& state) { state.fail_connect = false; });
    settings.host = "new-identity";
    const auto changed = configuration.prepare(settings);
    CHECK(changed && !*changed);
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    factory.inspect([](Control& state) { state.fail_probe.insert(5); });
    settings.host = "another-identity";
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    CHECK(factory.inspect([](const Control& state) { return state.attempts.size() == 6; }));
}

void silence_original_error_recovery_tests() {
    Reader reader;
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 50ms, 5s);
    auto settings = enabled_settings();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    factory.inspect([](Control& state) {
        state.fail_silence = true;
        state.silence_error_code = "2013";
        state.fail_probe.insert(1);
    });
    wait_for([&] { return configuration.read_last_received(true, 1s).status == LogSilenceReadStatus::error; });
    wait_for([&] { return factory.inspect([](const Control& state) { return state.probes.at(1) >= 2; }); });
    const auto failed = configuration.read_last_received(true, 1s);
    CHECK(failed.error && failed.error->code == "2013" && failed.queried_at && !failed.batch);
    factory.inspect([](Control& state) { state.blocked = true; });
    const auto reconnecting = configuration.prepare(settings);
    CHECK(!reconnecting && reconnecting.error().code == "2013");
    wait_for([&] { return factory.inspect([](const Control& state) { return state.attempts.size() == 2; }); });
    const auto retrying = configuration.read_last_received(true, 1s);
    CHECK(retrying.error->code == "2013" && retrying.queried_at == failed.queried_at);
    factory.inspect([](Control& state) {
        state.fail_silence = false;
        state.silence_blocked = true;
    });
    factory.release();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    wait_for([&] { return factory.inspect([](const Control& state) { return state.silence_queries.size() == 2; }); });
    CHECK(factory.inspect([](const Control& state) {
        return state.silence_query_times[1] - state.silence_query_times[0] >= 1s;
    }));
    const auto awaiting_query = configuration.read_last_received(true, 1s);
    CHECK(awaiting_query.status == LogSilenceReadStatus::error && !awaiting_query.batch);
    CHECK(awaiting_query.error->code == "2013" && awaiting_query.queried_at == failed.queried_at);
    factory.release_silence();
    wait_for([&] { return configuration.read_last_received(true, 1s).status == LogSilenceReadStatus::ready; });
    const auto recovered = configuration.read_last_received(true, 1s);
    CHECK(recovered.batch && !recovered.error && recovered.queried_at > failed.queried_at);
    factory.inspect([](Control& state) {
        state.fail_silence = true;
        state.silence_error_code = "1142";
    });
    wait_for([&] { return configuration.read_last_received(true, 1s).status == LogSilenceReadStatus::error; });
    settings.enabled = false;
    CHECK(configuration.prepare(settings).value());
    CHECK(configuration.commit());
    const auto disabled = configuration.read_last_received(false, 1s);
    CHECK(!disabled.error && !disabled.batch && !disabled.queried_at);
}

void silence_changed_identity_clears_error_tests() {
    Reader reader;
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 20ms, 5s);
    auto settings = enabled_settings();
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    factory.inspect([](Control& state) { state.fail_silence = true; state.silence_error_code = "1142"; });
    wait_for([&] { return configuration.read_last_received(true, 60s).status == LogSilenceReadStatus::error; });
    settings.database = "replacement";
    wait_ready(configuration, settings);
    factory.inspect([](Control& state) { state.fail_silence = false; state.silence_blocked = true; });
    CHECK(configuration.commit());
    wait_for([&] { return factory.inspect([](const Control& state) { return state.silence_queries.size() == 2; }); });
    const auto pending = configuration.read_last_received(true, 60s);
    CHECK(pending.status == LogSilenceReadStatus::pending);
    CHECK(!pending.error && !pending.batch && !pending.queried_at);
    factory.release_silence();
    wait_for([&] { return configuration.read_last_received(true, 60s).status == LogSilenceReadStatus::ready; });
}

void initial_credentials_failure_tests() {
    Reader reader;
    reader.next = std::unexpected(Error{"mysql_credentials", "credentials", "invalid_json", "Invalid credentials"});
    Factory factory;
    MySqlConfiguration configuration(reader, factory, 20ms, 5s);
    const auto settings = enabled_settings();
    CHECK(!configuration.prepare(settings));
    const auto failed = configuration.read_last_received(true, 60s);
    CHECK(failed.status == LogSilenceReadStatus::error && failed.error->code == "invalid_json");
    CHECK(!failed.batch && !failed.queried_at);
    CHECK(factory.inspect([](const Control& state) { return state.attempts.empty(); }));
    reader.next = MySqlCredentials{"monitor", "fixed-password"};
    wait_ready(configuration, settings);
    CHECK(configuration.commit());
    wait_for([&] { return configuration.read_last_received(true, 60s).status == LogSilenceReadStatus::ready; });
    CHECK(!configuration.read_last_received(true, 60s).error);
}

void independent_channels_isolation_tests() {
    Reader daily_reader;
    Reader silence_reader;
    Factory daily_factory;
    Factory silence_factory;
    MySqlConfiguration daily(daily_reader, daily_factory, 20ms, 5s);
    MySqlConfiguration silence(silence_reader, silence_factory, 20ms, 5s);
    const auto settings = enabled_settings();
    wait_ready(daily, settings);
    CHECK(daily.commit());
    wait_ready(silence, settings);
    CHECK(silence.commit());
    silence_factory.inspect([](Control& state) {
        state.silence_blocked = true;
        state.fail_silence = true;
        state.silence_error_code = "2013";
        state.fail_probe.insert(1);
    });
    silence.read_last_received(true, 1s);
    wait_for([&] { return silence_factory.inspect([](const Control& state) { return state.silence_queries.size() == 1; }); });
    const DailyCountWindow window{20261018, 20261017};
    wait_for([&] { return daily.read_daily_counts(window, 1s).status == CollectCountReadStatus::ready; });
    daily_factory.inspect([](Control& state) { state.current_count = 150; });
    wait_for([&] {
        const auto current = daily.read_daily_counts(window, 1s);
        return current.status == CollectCountReadStatus::ready && current.counts.front().current_count == 150;
    });
    silence_factory.release_silence();
    wait_for([&] { return silence.read_last_received(true, 1s).status == LogSilenceReadStatus::error; });
    CHECK(!silence.read_last_received(true, 1s).batch);
    CHECK(daily.read_daily_counts(window, 1s).status == CollectCountReadStatus::ready);
    CHECK(daily.prepare(settings).value());
    CHECK(daily_factory.inspect([](const Control& state) { return state.attempts.size() == 1; }));
}

}

int main() {
    return test::run([] {
        pending_and_rotation_tests();
        failure_and_retry_tests();
        stale_candidate_tests();
        health_recovery_tests();
        failed_probe_and_cancel_tests();
        disabled_health_and_retry_tests();
        expired_candidate_tests();
        exception_redaction_tests();
        query_initialization_and_refresh_tests();
        query_disable_and_window_change_tests();
        query_connection_rotation_tests();
        query_error_and_recovery_tests();
        query_stale_cache_tests();
        query_interval_reload_tests();
        silence_refresh_and_cancel_tests();
        silence_rotation_tests();
        silence_error_isolation_tests();
        silence_stale_cache_tests();
        silence_health_invalidation_tests();
        connection_error_survives_retry_tests();
        reconnect_backoff_tests();
        silence_original_error_recovery_tests();
        silence_changed_identity_clears_error_tests();
        initial_credentials_failure_tests();
        independent_channels_isolation_tests();
    });
}
