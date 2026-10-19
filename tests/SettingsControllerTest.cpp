// tests/SettingsControllerTest.cpp
#include "TestSupport.hpp"
#include "application/SettingsController.hpp"

#include <utility>
#include <vector>

namespace {

using namespace monitor;

MonitorSettings settings(std::chrono::seconds interval = std::chrono::seconds{60},
    std::string directory = "/Monitor", double threshold = 80.0) {
    return MonitorSettings{ThresholdPolicy{threshold, threshold, threshold}, interval,
        std::move(directory)};
}

struct Reader final : ISettingsReader {
    std::expected<MonitorSettings, Error> next = settings();
    std::size_t reads{};

    std::expected<MonitorSettings, Error> read_settings() override {
        ++reads;
        return next;
    }
};

struct Output final : IOutputDirectory {
    std::vector<std::string> attempts;
    std::optional<Error> failure;
    std::string active;

    std::expected<void, Error> configure_output(const std::string& directory) override {
        attempts.push_back(directory);
        if (failure) {
            return std::unexpected(*failure);
        }
        active = directory;
        return {};
    }
};

struct Database final : IMySqlConfiguration {
    std::expected<bool, Error> prepared{true};
    MySqlSettings candidate;
    MySqlSettings active;
    std::size_t commits{};
    bool credential_changed{false};

    std::expected<bool, Error> prepare(const MySqlSettings& next) override {
        candidate = next;
        return prepared;
    }

    bool commit() noexcept override {
        ++commits;
        const bool changed = active != candidate || credential_changed;
        active = candidate;
        credential_changed = false;
        return changed;
    }
};

void check_error(const Error& actual, const Error& expected) {
    CHECK(actual.component == expected.component);
    CHECK(actual.target == expected.target);
    CHECK(actual.code == expected.code);
    CHECK(actual.message == expected.message);
}

void initialization_tests() {
    Reader reader;
    Output output;
    SettingsController controller(reader, output);
    CHECK(!controller.read());
    CHECK(controller.read().error().code == "not_initialized");
    CHECK(!controller.reload());
    CHECK(controller.reload().error().code == "not_initialized");
    CHECK(reader.reads == 0);
    bool rejected = false;
    try {
        static_cast<void>(controller.current());
    } catch (const std::logic_error&) {
        rejected = true;
    }
    CHECK(rejected);

    const Error invalid{"settings_config", "settings.json", "invalid_json", "Invalid JSON"};
    reader.next = std::unexpected(invalid);
    auto initialized = controller.initialize();
    CHECK(!initialized);
    check_error(initialized.error(), invalid);
    CHECK(output.attempts.empty());
    CHECK(controller.read().error().code == "not_initialized");

    reader.next = settings(std::chrono::seconds{5}, "/snapshots", 90.0);
    const Error denied{"snapshot_store", "/snapshots", "permission_denied", "Access denied"};
    output.failure = denied;
    initialized = controller.initialize();
    CHECK(!initialized);
    check_error(initialized.error(), denied);
    CHECK(output.active.empty());
    CHECK(controller.read().error().code == "not_initialized");

    output.failure.reset();
    CHECK(controller.initialize());
    CHECK(controller.current() == *reader.next);
    CHECK(output.active == "/snapshots");
    CHECK(output.attempts.size() == 2);
    CHECK(*controller.read() == reader.next->thresholds);
    CHECK(!controller.reload_error());
    const auto reads = reader.reads;
    CHECK(!controller.initialize());
    CHECK(reader.reads == reads);
    CHECK(output.attempts.size() == 2);
}

void change_tests() {
    Reader reader;
    Output output;
    SettingsController controller(reader, output);
    CHECK(controller.initialize());
    auto result = controller.reload();
    CHECK(result);
    CHECK(!*result);
    CHECK(output.attempts.size() == 1);

    reader.next = settings(std::chrono::seconds{60}, "/Monitor", 90.0);
    result = controller.reload();
    CHECK(result);
    CHECK(*result);
    CHECK(controller.current().collection_interval == std::chrono::seconds{60});
    CHECK(output.attempts.size() == 1);
    CHECK(*controller.read() == reader.next->thresholds);

    reader.next = settings(std::chrono::seconds{3}, "/Monitor", 90.0);
    result = controller.reload();
    CHECK(result);
    CHECK(*result);
    CHECK(controller.current() == *reader.next);
    CHECK(output.attempts.size() == 1);

    reader.next = settings(std::chrono::seconds{7}, "/other", 75.0);
    result = controller.reload();
    CHECK(result);
    CHECK(*result);
    CHECK(controller.current() == *reader.next);
    CHECK(output.active == "/other");
    CHECK(output.attempts.size() == 2);
    const auto reads = reader.reads;
    CHECK(*controller.read() == reader.next->thresholds);
    CHECK(reader.reads == reads);
}

void read_failure_tests() {
    Reader reader;
    Output output;
    SettingsController controller(reader, output);
    CHECK(controller.initialize());
    const auto previous = controller.current();
    const Error failure{"settings_config", "settings.json", "invalid_json", "Truncated JSON"};
    reader.next = std::unexpected(failure);
    auto result = controller.reload();
    CHECK(!result);
    check_error(result.error(), failure);
    CHECK(controller.current() == previous);
    CHECK(output.attempts.size() == 1);
    const auto policy = controller.read();
    CHECK(policy);
    CHECK(*policy == previous.thresholds);
    CHECK(controller.reload_error());
    check_error(*controller.reload_error(), failure);

    reader.next = previous;
    result = controller.reload();
    CHECK(result);
    CHECK(!*result);
    CHECK(*controller.read() == previous.thresholds);
    CHECK(!controller.reload_error());
    CHECK(output.attempts.size() == 1);
}

void output_failure_tests() {
    Reader reader;
    Output output;
    SettingsController controller(reader, output);
    CHECK(controller.initialize());
    const auto previous = controller.current();
    reader.next = settings(std::chrono::seconds{2}, "/new", 0.0);
    const Error failure{"snapshot_store", "/new", "directory_not_found", "Directory unavailable"};
    output.failure = failure;
    auto result = controller.reload();
    CHECK(!result);
    check_error(result.error(), failure);
    CHECK(controller.current() == previous);
    CHECK(output.active == previous.output_directory);
    const auto policy = controller.read();
    CHECK(policy);
    CHECK(*policy == previous.thresholds);
    CHECK(controller.reload_error());
    check_error(*controller.reload_error(), failure);

    output.failure.reset();
    result = controller.reload();
    CHECK(result);
    CHECK(*result);
    CHECK(controller.current() == *reader.next);
    CHECK(output.active == "/new");
    CHECK(output.attempts.size() == 3);
    CHECK(*controller.read() == reader.next->thresholds);
    CHECK(!controller.reload_error());
}

void override_tests() {
    {
        Reader reader;
        Output output;
        SettingsOverrides overrides;
        overrides.interval = std::chrono::seconds{4};
        SettingsController controller(reader, output, overrides);
        CHECK(controller.initialize());
        CHECK(controller.current().collection_interval == *overrides.interval);
        CHECK(controller.current().output_directory == "/Monitor");
        reader.next = settings(std::chrono::seconds{9}, "/Monitor");
        const auto unchanged = controller.reload();
        CHECK(unchanged);
        CHECK(!*unchanged);
        reader.next = settings(std::chrono::seconds{9}, "/changed", 40.0);
        const auto changed = controller.reload();
        CHECK(changed);
        CHECK(*changed);
        CHECK(controller.current().collection_interval == *overrides.interval);
        CHECK(controller.current().output_directory == "/changed");
        CHECK(*controller.read() == reader.next->thresholds);
        CHECK(output.attempts.size() == 2);
    }
    {
        Reader reader;
        Output output;
        SettingsOverrides overrides;
        overrides.output_directory = "/command-line";
        SettingsController controller(reader, output, overrides);
        CHECK(controller.initialize());
        CHECK(controller.current().collection_interval == std::chrono::seconds{60});
        CHECK(output.active == *overrides.output_directory);
        reader.next = settings(std::chrono::seconds{60}, "/ignored");
        const auto unchanged = controller.reload();
        CHECK(unchanged);
        CHECK(!*unchanged);
        reader.next = settings(std::chrono::seconds{3}, "/ignored", 20.0);
        const auto changed = controller.reload();
        CHECK(changed);
        CHECK(*changed);
        CHECK(controller.current().collection_interval == std::chrono::seconds{3});
        CHECK(controller.current().output_directory == *overrides.output_directory);
        CHECK(*controller.read() == reader.next->thresholds);
        CHECK(output.attempts.size() == 1);
    }
    {
        Reader reader;
        Output output;
        SettingsOverrides overrides;
        overrides.interval = std::chrono::seconds{2};
        overrides.output_directory = "/fixed";
        SettingsController controller(reader, output, overrides);
        CHECK(controller.initialize());
        reader.next = settings(std::chrono::seconds{10}, "/ignored");
        const auto unchanged = controller.reload();
        CHECK(unchanged);
        CHECK(!*unchanged);
        CHECK(controller.current().collection_interval == *overrides.interval);
        CHECK(controller.current().output_directory == *overrides.output_directory);
        CHECK(output.attempts.size() == 1);
        const Error failure{"settings_config", "settings.json", "invalid_interval", "Invalid interval"};
        reader.next = std::unexpected(failure);
        CHECK(!controller.reload());
        CHECK(*controller.read() == controller.current().thresholds);
        CHECK(controller.reload_error());
        check_error(*controller.reload_error(), failure);
    }
}

void database_reload_tests() {
    Reader reader;
    Output output;
    Database database;
    SettingsController controller(reader, output, {}, &database);
    CHECK(controller.initialize());
    const auto previous = controller.current();
    reader.next->is_analyzer = true;
    reader.next->mysql.enabled = true;
    reader.next->mysql.database = "monitoring";
    reader.next->collection_interval = std::chrono::seconds{2};
    reader.next->output_directory = "/new";
    database.prepared = false;
    CHECK(!controller.reload());
    CHECK(controller.current() == previous);
    CHECK(output.active == "/Monitor");
    CHECK(database.commits == 1);
    CHECK(controller.reload_error()->code == "connection_pending");
    database.prepared = std::unexpected(Error{"mysql", "connection", "connect_failed", "Connection failed"});
    CHECK(!controller.reload());
    CHECK(controller.current() == previous);
    CHECK(database.commits == 1);
    database.prepared = true;
    output.failure = Error{"snapshot_store", "/new", "io_error", "Access denied"};
    CHECK(!controller.reload());
    CHECK(controller.current() == previous);
    CHECK(!database.active.enabled);
    CHECK(database.commits == 1);
    output.failure.reset();
    auto changed = controller.reload();
    CHECK(changed && *changed);
    CHECK(controller.current() == *reader.next);
    CHECK(database.active == reader.next->mysql);
    CHECK(output.active == "/new");
    CHECK(!controller.reload_error());
    changed = controller.reload();
    CHECK(changed && !*changed);
    database.credential_changed = true;
    changed = controller.reload();
    CHECK(changed && *changed);
    CHECK(controller.current() == *reader.next);
    reader.next->mysql.enabled = false;
    changed = controller.reload();
    CHECK(changed && *changed);
    CHECK(!database.active.enabled);
}

void database_startup_tests() {
    for (const bool pending : {true, false}) {
        Reader reader;
        reader.next->is_analyzer = true;
        reader.next->mysql.enabled = true;
        reader.next->mysql.database = "monitoring";
        Output output;
        Database database;
        if (pending) {
            database.prepared = false;
        } else {
            database.prepared = std::unexpected(Error{"mysql_credentials", "credentials", "invalid_json", "Invalid credentials"});
        }
        SettingsController controller(reader, output, {}, &database);
        CHECK(controller.initialize());
        CHECK(!controller.current().mysql.enabled);
        CHECK(controller.current().thresholds == reader.next->thresholds);
        CHECK(output.active == "/Monitor");
        CHECK(controller.reload_error());
        CHECK(database.commits == 0);
        database.prepared = true;
        const auto changed = controller.reload();
        CHECK(changed && *changed);
        CHECK(controller.current().mysql.enabled);
        CHECK(database.active.enabled);
        CHECK(!controller.reload_error());
    }
    Reader reader;
    reader.next->is_analyzer = true;
    reader.next->mysql.enabled = true;
    Output output;
    SettingsController controller(reader, output);
    CHECK(!controller.initialize());
    CHECK(output.attempts.empty());
}

void analyzer_gate_tests() {
    for (const bool analyzer : {false, true}) {
        for (const bool enabled : {false, true}) {
            Reader reader;
            reader.next->is_analyzer = analyzer;
            reader.next->mysql.enabled = enabled;
            Output output;
            Database database;
            SettingsController controller(reader, output, {}, &database);
            CHECK(controller.initialize());
            CHECK(controller.current().is_analyzer == analyzer);
            CHECK(controller.current().mysql.enabled == (analyzer && enabled));
            CHECK(database.candidate.enabled == (analyzer && enabled));
            CHECK(database.active.enabled == (analyzer && enabled));
            CHECK(!controller.reload_error());
            const auto unchanged = controller.reload();
            CHECK(unchanged && !*unchanged);
        }
    }

    Reader reader;
    reader.next->mysql.enabled = true;
    reader.next->mysql.database = "monitoring";
    Output output;
    Database database;
    SettingsController controller(reader, output, {}, &database);
    CHECK(controller.initialize());
    CHECK(!controller.current().is_analyzer);
    CHECK(!controller.current().mysql.enabled);
    CHECK(reader.next->mysql.enabled);
    CHECK(!database.active.enabled);

    reader.next->is_analyzer = true;
    database.prepared = false;
    CHECK(!controller.reload());
    CHECK(!controller.current().is_analyzer);
    CHECK(!controller.current().mysql.enabled);
    CHECK(controller.reload_error()->code == "connection_pending");
    database.prepared = true;
    auto changed = controller.reload();
    CHECK(changed && *changed);
    CHECK(controller.current().is_analyzer);
    CHECK(controller.current().mysql.enabled);
    CHECK(database.active.enabled);

    reader.next->is_analyzer = false;
    changed = controller.reload();
    CHECK(changed && *changed);
    CHECK(!controller.current().is_analyzer);
    CHECK(!controller.current().mysql.enabled);
    CHECK(!database.active.enabled);
    CHECK(reader.next->mysql.enabled);
    CHECK(!controller.reload_error());
    changed = controller.reload();
    CHECK(changed && !*changed);

    reader.next->is_analyzer = true;
    changed = controller.reload();
    CHECK(changed && *changed);
    CHECK(database.active.enabled);
    reader.next->mysql.enabled = false;
    changed = controller.reload();
    CHECK(changed && *changed);
    CHECK(controller.current().is_analyzer);
    CHECK(!controller.current().mysql.enabled);
    CHECK(!database.active.enabled);

    Reader without_adapter;
    without_adapter.next->mysql.enabled = true;
    Output independent_output;
    SettingsController independent(without_adapter, independent_output);
    CHECK(independent.initialize());
    CHECK(!independent.current().mysql.enabled);
    CHECK(!independent.reload_error());
    CHECK(independent_output.active == "/Monitor");
}

}

int main() {
    return test::run([] {
        initialization_tests();
        change_tests();
        read_failure_tests();
        output_failure_tests();
        override_tests();
        database_reload_tests();
        database_startup_tests();
        analyzer_gate_tests();
    });
}
