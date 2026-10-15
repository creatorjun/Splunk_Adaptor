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

}

int main() {
    return test::run([] {
        initialization_tests();
        change_tests();
        read_failure_tests();
        output_failure_tests();
        override_tests();
    });
}
