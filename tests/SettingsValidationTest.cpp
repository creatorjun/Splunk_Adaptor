// tests/SettingsValidationTest.cpp
#include "TestSupport.hpp"
#include "application/SettingsValidation.hpp"

#include <limits>
#include <utility>
#include <vector>

namespace {

using namespace monitor;
using namespace std::chrono_literals;

void valid_settings_tests() {
    MonitorSettings settings;
    CHECK(validate_settings(settings));
    settings.collection_interval = 1s;
    settings.processing_files.collector = {0, 1000000000000ULL};
    settings.retention.max_age = SnapshotRetentionSettings::minimum_age;
    CHECK(validate_settings(settings));
    settings.collection_interval = 86400s;
    settings.retention.max_age = SnapshotRetentionSettings::maximum_age;
    settings.role = ServerRole::prx;
    settings.mysql.enabled = true;
    CHECK(validate_settings(settings));
    settings.role = ServerRole::anal;
    settings.mysql.database = "CloudESM";
    settings.mysql.tls_ca_file = "/etc/ca.pem";
    CHECK(validate_settings(settings));
    settings.mysql.tls_mode = "disabled";
    settings.mysql.tls_ca_file.clear();
    CHECK(validate_settings(settings));
    settings.process_monitor.col = {"Collector", "collector", "Collector Agent", "\uC218\uC9D1\uAE30"};
    CHECK(validate_settings(settings));
}

void invalid_settings_tests() {
    using Mutation = void (*)(MonitorSettings&);
    const std::vector<std::pair<Mutation, const char*>> cases{
        {[](auto& value) { value.role = static_cast<ServerRole>(99); }, "invalid_role"},
        {[](auto& value) { value.thresholds.cpu_usage_percent = std::numeric_limits<double>::quiet_NaN(); }, "invalid_threshold"},
        {[](auto& value) { value.collection_interval = 0s; }, "invalid_interval"},
        {[](auto& value) { value.collection_interval = 86401s; }, "invalid_interval"},
        {[](auto& value) { value.output_directory = "relative"; }, "invalid_output_directory"},
        {[](auto& value) { value.output_directory = std::string{"/hidden\0suffix", 14}; }, "invalid_output_directory"},
        {[](auto& value) { value.retention.max_age = 59s; }, "invalid_retention"},
        {[](auto& value) { value.retention.enabled = false; value.retention.max_age = 31536001s; }, "invalid_retention"},
        {[](auto& value) { value.process_monitor.anal = {"Repeated", "Repeated"}; }, "invalid_process_monitor"},
        {[](auto& value) { value.process_monitor.col = {" Trailing"}; }, "invalid_process_monitor"},
        {[](auto& value) { value.process_monitor.prx = {"control\xc2\x80"}; }, "invalid_process_monitor"},
        {[](auto& value) { value.process_monitor.prx = {std::string(129, 'x')}; }, "invalid_process_monitor"},
        {[](auto& value) { value.processing_files.normalize.done_count = 1000000000001ULL; }, "invalid_processing_files"},
        {[](auto& value) { value.relay_log.path = "/logs/"; }, "invalid_relay_log"},
        {[](auto& value) { value.relay_log.path = "/logs/.."; }, "invalid_relay_log"},
        {[](auto& value) { value.mysql.host.clear(); }, "invalid_mysql"},
        {[](auto& value) { value.mysql.port = 65536; }, "invalid_mysql"},
        {[](auto& value) { value.mysql.read_timeout_seconds = 0; }, "invalid_mysql"},
        {[](auto& value) { value.mysql.credentials_file = "relative.json"; }, "invalid_mysql"},
        {[](auto& value) { value.mysql.database = "redaction-marker\n"; }, "invalid_mysql"},
        {[](auto& value) { value.mysql.tls_mode = "unknown"; }, "invalid_mysql"},
        {[](auto& value) { value.mysql.tls_ca_file = "relative.pem"; }, "invalid_mysql"},
        {[](auto& value) { value.role = ServerRole::anal; }, "invalid_mysql"},
        {[](auto& value) { value.role = ServerRole::anal; value.mysql.database = "CloudESM"; }, "invalid_mysql"}
    };
    for (const auto& [mutate, code] : cases) {
        MonitorSettings settings;
        mutate(settings);
        const auto result = validate_settings(settings);
        CHECK(!result && result.error().code == code);
        CHECK(result.error().component == "settings_config");
        CHECK(result.error().message.find("redaction-marker") == std::string::npos);
    }
}

}

int main() {
    return test::run([] {
        valid_settings_tests();
        invalid_settings_tests();
    });
}
