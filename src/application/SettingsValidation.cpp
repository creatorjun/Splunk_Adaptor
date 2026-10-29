// src/application/SettingsValidation.cpp
#include "application/SettingsValidation.hpp"

#include <array>
#include <cstdint>
#include <set>
#include <string_view>
#include <utility>

namespace monitor {
namespace {

Error settings_error(std::string code, std::string message) {
    return {"settings_config", "settings", std::move(code), std::move(message)};
}

bool has_control_characters(std::string_view value) {
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto character = static_cast<unsigned char>(value[index]);
        if (character < 0x20 || character == 0x7f) return true;
        if (character == 0xc2 && index + 1 < value.size()) {
            const auto next = static_cast<unsigned char>(value[index + 1]);
            if (next >= 0x80 && next <= 0x9f) return true;
        }
    }
    return false;
}

bool absolute_path(std::string_view path) {
    return !path.empty() && path.front() == '/' && !has_control_characters(path);
}

bool valid_process_apps(const std::vector<std::string>& apps) {
    if (apps.size() > 256) return false;
    std::set<std::string_view> unique;
    for (const auto& app : apps) {
        if (app.empty() || app.size() > 128 || has_control_characters(app) ||
            app.front() == ' ' || app.back() == ' ' || !unique.insert(app).second) return false;
    }
    return true;
}

}

std::expected<void, Error> validate_settings(const MonitorSettings& settings) {
    if (auto valid = validate_thresholds(settings.thresholds); !valid) {
        auto error = std::move(valid.error());
        error.component = "settings_config";
        error.target = "settings";
        return std::unexpected(std::move(error));
    }
    if (settings.role != ServerRole::col && settings.role != ServerRole::anal && settings.role != ServerRole::prx) {
        return std::unexpected(settings_error("invalid_role", "role must be col, anal or prx"));
    }
    if (settings.retention.max_age < SnapshotRetentionSettings::minimum_age ||
        settings.retention.max_age > SnapshotRetentionSettings::maximum_age) {
        return std::unexpected(settings_error("invalid_retention",
            "Retention must contain only enabled (boolean) and max_age_seconds (integer between 60 and 31536000)"));
    }
    if (!valid_process_apps(settings.process_monitor.anal) || !valid_process_apps(settings.process_monitor.col) ||
        !valid_process_apps(settings.process_monitor.prx)) {
        return std::unexpected(settings_error("invalid_process_monitor",
            "Process lists must contain at most 256 unique names, each 1..128 UTF-8 bytes without control characters or leading or trailing ASCII whitespace"));
    }
    constexpr std::uint64_t maximum_file_count = 1000000000000ULL;
    for (const auto& counts : {settings.processing_files.collector, settings.processing_files.normalize}) {
        if (counts.tmp_count > maximum_file_count || counts.done_count > maximum_file_count) {
            return std::unexpected(settings_error("invalid_processing_files",
                "Processing file thresholds must be objects with collector and normalize integer counts between 0 and 1000000000000"));
        }
    }
    if (settings.collection_interval < std::chrono::seconds{1} || settings.collection_interval > std::chrono::seconds{86400}) {
        return std::unexpected(settings_error("invalid_interval", "interval_seconds must be an integer between 1 and 86400"));
    }
    if (!absolute_path(settings.output_directory)) {
        return std::unexpected(settings_error("invalid_output_directory", "output_directory must be an absolute path without control characters"));
    }
    const std::string_view relay_path{settings.relay_log.path};
    const auto filename = relay_path.substr(relay_path.find_last_of('/') + 1);
    if (!absolute_path(relay_path) || filename.empty() || filename == "." || filename == "..") {
        return std::unexpected(settings_error("invalid_relay_log",
            "relay_log.path must be an absolute file path without control characters"));
    }
    const auto& mysql = settings.mysql;
    const std::array<std::string_view, 5> mysql_strings{
        mysql.host, mysql.database, mysql.credentials_file, mysql.tls_mode, mysql.tls_ca_file};
    for (const auto value : mysql_strings) {
        if (has_control_characters(value)) {
            return std::unexpected(settings_error("invalid_mysql", "MySQL settings must not contain control characters"));
        }
    }
    if (mysql.port < 1 || mysql.port > 65535 || mysql.connect_timeout_seconds < 1 || mysql.connect_timeout_seconds > 10 ||
        mysql.read_timeout_seconds < 1 || mysql.read_timeout_seconds > 10 || mysql.write_timeout_seconds < 1 || mysql.write_timeout_seconds > 10) {
        return std::unexpected(settings_error("invalid_mysql", "MySQL port must be 1..65535 and timeouts must be 1..10 integer seconds"));
    }
    if (mysql.host.empty() || (settings.role == ServerRole::anal && mysql.database.empty())) {
        return std::unexpected(settings_error("invalid_mysql", "MySQL host is required and database is required when enabled"));
    }
    if (!absolute_path(mysql.credentials_file)) {
        return std::unexpected(settings_error("invalid_mysql", "MySQL credentials_file must be an absolute path"));
    }
    if (mysql.tls_mode != "disabled" && mysql.tls_mode != "verify_identity") {
        return std::unexpected(settings_error("invalid_mysql", "MySQL tls_mode must be disabled or verify_identity"));
    }
    if ((!mysql.tls_ca_file.empty() && !absolute_path(mysql.tls_ca_file)) ||
        (settings.role == ServerRole::anal && mysql.tls_mode == "verify_identity" && mysql.tls_ca_file.empty())) {
        return std::unexpected(settings_error("invalid_mysql", "MySQL tls_ca_file must be absolute and is required when enabled with verify_identity"));
    }
    return {};
}

}
