// src/infrastructure/config/JsonSettingsReader.cpp
#include "infrastructure/config/JsonSettingsReader.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <set>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace monitor {
namespace {

class ConfigFd {
public:
    explicit ConfigFd(int value) noexcept : value_(value) {}
    ~ConfigFd() { if (value_ >= 0) ::close(value_); }
    ConfigFd(const ConfigFd&) = delete;
    ConfigFd& operator=(const ConfigFd&) = delete;
    int get() const noexcept { return value_; }

private:
    int value_;
};

Error config_error(const std::filesystem::path& path, std::string code, std::string message) {
    return {"settings_config", path.string(), std::move(code), std::move(message)};
}

Error io_error(const std::filesystem::path& path, const char* operation, int error) {
    return config_error(path, "io_error", std::string(operation) + ": " +
        std::error_code(error, std::generic_category()).message());
}

bool has_control_characters(const std::string& value) {
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

}

JsonSettingsReader::JsonSettingsReader(std::filesystem::path path)
    : path_(std::move(path)) {}

std::expected<ThresholdPolicy, Error> JsonSettingsReader::read() {
    auto settings = read_settings();
    if (!settings) return std::unexpected(std::move(settings.error()));
    return settings->thresholds;
}

std::expected<MonitorSettings, Error> JsonSettingsReader::read_settings() {
    constexpr std::size_t maximum_bytes = 64 * 1024;
    const ConfigFd file(::open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW));
    if (file.get() < 0) {
        return std::unexpected(io_error(path_, "open", errno));
    }
    struct stat metadata {};
    if (::fstat(file.get(), &metadata) < 0) {
        return std::unexpected(io_error(path_, "fstat", errno));
    }
    if (!S_ISREG(metadata.st_mode)) {
        return std::unexpected(config_error(path_, "invalid_file", "Configuration must be a regular file"));
    }
    if (metadata.st_size < 0 || static_cast<std::uintmax_t>(metadata.st_size) > maximum_bytes) {
        return std::unexpected(config_error(path_, "file_too_large", "Configuration exceeds 65536 bytes"));
    }
    std::string document;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(io_error(path_, "read", errno));
        }
        if (count == 0) break;
        const auto amount = static_cast<std::size_t>(count);
        if (amount > maximum_bytes - document.size()) {
            return std::unexpected(config_error(path_, "file_too_large", "Configuration exceeds 65536 bytes"));
        }
        document.append(buffer.data(), amount);
    }

    try {
        std::vector<std::set<std::string>> object_keys;
        const auto callback = [&object_keys](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value) {
            if (depth > 16) throw std::invalid_argument("JSON nesting exceeds 16 levels");
            if (event == nlohmann::json::parse_event_t::object_start) {
                object_keys.emplace_back();
            } else if (event == nlohmann::json::parse_event_t::object_end) {
                object_keys.pop_back();
            } else if (event == nlohmann::json::parse_event_t::key) {
                if (!object_keys.back().insert(value.get<std::string>()).second) {
                    throw std::invalid_argument("Duplicate JSON object key");
                }
            }
            return true;
        };
        const auto json = nlohmann::json::parse(document, callback);
        if (!json.is_object() || !json.contains("schema_version") ||
            !json.contains("thresholds")) {
            return std::unexpected(config_error(path_, "invalid_schema", "Expected schema_version, thresholds, and optional is_analyzer, collection and mysql"));
        }
        for (const auto& [key, value] : json.items()) {
            if (key != "schema_version" && key != "thresholds" && key != "collection" && key != "mysql" && key != "is_analyzer") {
                return std::unexpected(config_error(path_, "invalid_schema", "Unknown configuration key"));
            }
        }
        if (!json["schema_version"].is_number_integer() || json["schema_version"] != 1) {
            return std::unexpected(config_error(path_, "invalid_schema", "schema_version must be the integer 1"));
        }
        const auto& thresholds = json["thresholds"];
        constexpr std::array keys{"cpu_usage_percent", "memory_usage_percent", "storage_usage_percent"};
        if (!thresholds.is_object() || thresholds.size() != keys.size()) {
            return std::unexpected(config_error(path_, "invalid_schema", "Expected exactly three resource thresholds"));
        }
        for (const auto* key : keys) {
            if (!thresholds.contains(key) || !thresholds[key].is_number()) {
                return std::unexpected(config_error(path_, "invalid_threshold", std::string(key) + " must be a number"));
            }
        }
        const ThresholdPolicy policy{
            thresholds[keys[0]].get<double>(),
            thresholds[keys[1]].get<double>(),
            thresholds[keys[2]].get<double>()
        };
        if (auto valid = validate_thresholds(policy); !valid) {
            auto error = valid.error();
            error.component = "settings_config";
            error.target = path_.string();
            return std::unexpected(std::move(error));
        }
        MonitorSettings settings{policy};
        if (json.contains("is_analyzer")) {
            if (!json["is_analyzer"].is_boolean()) {
                return std::unexpected(config_error(path_, "invalid_schema", "is_analyzer must be a boolean"));
            }
            settings.is_analyzer = json["is_analyzer"].get<bool>();
        }
        if (json.contains("collection")) {
            const auto& collection = json["collection"];
            if (!collection.is_object()) {
                return std::unexpected(config_error(path_, "invalid_schema", "collection must be an object"));
            }
            for (const auto& [key, value] : collection.items()) {
                if (key != "interval_seconds" && key != "output_directory") {
                    return std::unexpected(config_error(path_, "invalid_schema", "Unknown collection key"));
                }
            }
            if (collection.contains("interval_seconds")) {
                const auto& interval = collection["interval_seconds"];
                if (!interval.is_number_integer()) {
                    return std::unexpected(config_error(path_, "invalid_interval", "interval_seconds must be an integer between 1 and 86400"));
                }
                std::int64_t seconds{};
                if (interval.is_number_unsigned()) {
                    const auto value = interval.get<std::uint64_t>();
                    if (value < 1 || value > 86400) {
                        return std::unexpected(config_error(path_, "invalid_interval", "interval_seconds must be an integer between 1 and 86400"));
                    }
                    seconds = static_cast<std::int64_t>(value);
                } else {
                    seconds = interval.get<std::int64_t>();
                    if (seconds < 1 || seconds > 86400) {
                        return std::unexpected(config_error(path_, "invalid_interval", "interval_seconds must be an integer between 1 and 86400"));
                    }
                }
                settings.collection_interval = std::chrono::seconds{seconds};
            }
            if (collection.contains("output_directory")) {
                const auto& output = collection["output_directory"];
                if (!output.is_string()) {
                    return std::unexpected(config_error(path_, "invalid_output_directory", "output_directory must be an absolute path without control characters"));
                }
                const auto& value = output.get_ref<const std::string&>();
                if (value.empty() || has_control_characters(value) || !std::filesystem::path(value).is_absolute()) {
                    return std::unexpected(config_error(path_, "invalid_output_directory", "output_directory must be an absolute path without control characters"));
                }
                settings.output_directory = value;
            }
        }
        if (json.contains("mysql")) {
            const auto& mysql = json["mysql"];
            if (!mysql.is_object()) {
                return std::unexpected(config_error(path_, "invalid_mysql", "mysql must be an object"));
            }
            const std::set<std::string> allowed_keys{
                "enabled", "host", "port", "database", "credentials_file", "connect_timeout_seconds",
                "read_timeout_seconds", "write_timeout_seconds", "tls_mode", "tls_ca_file"
            };
            for (const auto& [key, value] : mysql.items()) {
                if (!allowed_keys.contains(key)) {
                    return std::unexpected(config_error(path_, "invalid_mysql", "Unknown MySQL setting; credentials must be stored in the separate credentials file"));
                }
            }
            if (mysql.contains("enabled")) {
                if (!mysql["enabled"].is_boolean()) {
                    return std::unexpected(config_error(path_, "invalid_mysql", "mysql.enabled must be a boolean"));
                }
                settings.mysql.enabled = mysql["enabled"].get<bool>();
            }
            const std::array string_fields{
                std::pair{"host", &settings.mysql.host},
                std::pair{"database", &settings.mysql.database},
                std::pair{"credentials_file", &settings.mysql.credentials_file},
                std::pair{"tls_mode", &settings.mysql.tls_mode},
                std::pair{"tls_ca_file", &settings.mysql.tls_ca_file}
            };
            for (const auto& [key, destination] : string_fields) {
                if (!mysql.contains(key)) continue;
                if (!mysql[key].is_string()) {
                    return std::unexpected(config_error(path_, "invalid_mysql", std::string("mysql.") + key + " must be a string"));
                }
                *destination = mysql[key].get<std::string>();
                if (has_control_characters(*destination)) {
                    return std::unexpected(config_error(path_, "invalid_mysql", "MySQL settings must not contain control characters"));
                }
            }
            const std::array numeric_fields{
                std::pair{"port", &settings.mysql.port},
                std::pair{"connect_timeout_seconds", &settings.mysql.connect_timeout_seconds},
                std::pair{"read_timeout_seconds", &settings.mysql.read_timeout_seconds},
                std::pair{"write_timeout_seconds", &settings.mysql.write_timeout_seconds}
            };
            for (const auto& [key, destination] : numeric_fields) {
                if (!mysql.contains(key)) continue;
                const auto& number = mysql[key];
                const auto maximum = std::string_view(key) == "port" ? 65535U : 10U;
                if (!number.is_number_integer() ||
                    (number.is_number_unsigned() && (number.get<std::uint64_t>() < 1 || number.get<std::uint64_t>() > maximum)) ||
                    (!number.is_number_unsigned() && (number.get<std::int64_t>() < 1 || number.get<std::int64_t>() > maximum))) {
                    return std::unexpected(config_error(path_, "invalid_mysql", "MySQL port must be 1..65535 and timeouts must be 1..10 integer seconds"));
                }
                *destination = number.get<unsigned int>();
            }
            if (settings.mysql.host.empty() || (settings.mysql_enabled() && settings.mysql.database.empty())) {
                return std::unexpected(config_error(path_, "invalid_mysql", "MySQL host is required and database is required when enabled"));
            }
            if (settings.mysql.credentials_file.empty() || !std::filesystem::path(settings.mysql.credentials_file).is_absolute()) {
                return std::unexpected(config_error(path_, "invalid_mysql", "MySQL credentials_file must be an absolute path"));
            }
            if (settings.mysql.tls_mode != "disabled" && settings.mysql.tls_mode != "verify_identity") {
                return std::unexpected(config_error(path_, "invalid_mysql", "MySQL tls_mode must be disabled or verify_identity"));
            }
            if ((!settings.mysql.tls_ca_file.empty() && !std::filesystem::path(settings.mysql.tls_ca_file).is_absolute()) ||
                (settings.mysql_enabled() && settings.mysql.tls_mode == "verify_identity" && settings.mysql.tls_ca_file.empty())) {
                return std::unexpected(config_error(path_, "invalid_mysql", "MySQL tls_ca_file must be absolute and is required when enabled with verify_identity"));
            }
        }
        return settings;
    } catch (const nlohmann::json::exception&) {
        return std::unexpected(config_error(path_, "invalid_json", "Invalid settings JSON document"));
    } catch (const std::invalid_argument&) {
        return std::unexpected(config_error(path_, "invalid_json", "Invalid settings JSON structure"));
    }
}

}
