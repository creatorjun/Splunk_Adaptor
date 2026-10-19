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
};

struct Control {
    std::mutex mutex;
    std::condition_variable wake;
    bool blocked{};
    bool fail_connect{};
    bool throw_connect{};
    bool signals_masked{};
    std::vector<Attempt> attempts;
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
    });
}
