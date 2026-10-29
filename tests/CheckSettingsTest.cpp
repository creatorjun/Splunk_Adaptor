// tests/CheckSettingsTest.cpp
#include "TestSupport.hpp"
#include "application/CheckSettings.hpp"

namespace {

using namespace monitor;

struct Settings final : ISettingsReader {
    std::expected<MonitorSettings, Error> next{MonitorSettings{}};
    std::expected<MonitorSettings, Error> read_settings() override { return next; }
};

struct Credentials final : IMySqlCredentialsReader {
    std::expected<MySqlCredentials, Error> next{MySqlCredentials{"test", "placeholder"}};
    unsigned reads{};
    std::string last_path;
    std::expected<MySqlCredentials, Error> read_credentials(const std::string& path) override {
        ++reads;
        last_path = path;
        return next;
    }
};

void role_tests() {
    for (const auto role : {ServerRole::col, ServerRole::anal, ServerRole::prx}) {
        Settings settings;
        settings.next->role = role;
        settings.next->mysql.enabled = role != ServerRole::anal;
        settings.next->mysql.database = "CloudESM";
        settings.next->mysql.tls_mode = "disabled";
        settings.next->mysql.credentials_file = "/custom/credentials.json";
        Credentials credentials;
        const auto result = check_settings(settings, credentials);
        CHECK(result);
        CHECK(credentials.reads == (role == ServerRole::anal ? 1U : 0U));
        if (role == ServerRole::anal) CHECK(credentials.last_path == "/custom/credentials.json");
    }
}

void failure_tests() {
    Settings settings;
    Credentials credentials;
    settings.next = std::unexpected(Error{"settings_config", "file", "invalid_json", "Invalid JSON"});
    auto result = check_settings(settings, credentials);
    CHECK(!result && result.error().code == "invalid_json" && credentials.reads == 0);
    settings.next = MonitorSettings{};
    settings.next->role = ServerRole::anal;
    result = check_settings(settings, credentials);
    CHECK(!result && result.error().code == "invalid_mysql" && credentials.reads == 0);
    settings.next->mysql.database = "CloudESM";
    settings.next->mysql.tls_mode = "disabled";
    credentials.next = std::unexpected(Error{"mysql_credentials", "mysql", "insecure_permissions", "Restricted permissions required"});
    result = check_settings(settings, credentials);
    CHECK(!result && result.error().code == "insecure_permissions" && credentials.reads == 1);
    for (const auto role : {ServerRole::col, ServerRole::prx}) {
        settings.next->role = role;
        CHECK(check_settings(settings, credentials));
        CHECK(credentials.reads == 1);
    }
}

}

int main() {
    return test::run([] {
        role_tests();
        failure_tests();
    });
}
