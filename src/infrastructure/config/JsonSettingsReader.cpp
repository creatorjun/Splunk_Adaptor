// src/infrastructure/config/JsonSettingsReader.cpp
#include "infrastructure/config/JsonSettingsReader.hpp"
#include "application/SettingsValidation.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <set>
#include <stdexcept>
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

bool read_process_apps(const nlohmann::json& value, std::vector<std::string>& destination) {
    if (!value.is_array()) return false;
    std::vector<std::string> apps;
    apps.reserve(value.size());
    for (const auto& item : value) {
        if (!item.is_string()) return false;
        apps.push_back(item.get<std::string>());
    }
    destination = std::move(apps);
    return true;
}


bool read_retention(const nlohmann::json& value, SnapshotRetentionSettings& destination) {
    if (!value.is_object()) return false;
    for (const auto& [key, setting] : value.items()) {
        if (key == "enabled") {
            if (!setting.is_boolean()) return false;
            destination.enabled = setting.get<bool>();
        } else if (key == "max_age_seconds") {
            if (!setting.is_number_integer()) return false;
            if (setting.is_number_unsigned() && setting.get<std::uint64_t>() >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
            destination.max_age = std::chrono::seconds{setting.get<std::int64_t>()};
        } else {
            return false;
        }
    }
    return true;
}

bool read_processing_file_thresholds(const nlohmann::json& value, ProcessingFileThresholds& destination) {
    if (!value.is_object()) return false;
    for (const auto& [key, count] : value.items()) {
        if ((key != "tmp_count" && key != "done_count") || !count.is_number_integer()) return false;
        if (!count.is_number_unsigned() && count.get<std::int64_t>() < 0) return false;
        (key == "tmp_count" ? destination.tmp_count : destination.done_count) = count.get<std::uint64_t>();
    }
    return true;
}

bool read_processing_files(const nlohmann::json& value, ProcessingFileSettings& destination) {
    if (!value.is_object()) return false;
    for (const auto& [key, thresholds] : value.items()) {
        if (key != "thresholds" || !thresholds.is_object()) return false;
        for (const auto& [directory, counts] : thresholds.items()) {
            if (directory != "collector" && directory != "normalize") return false;
            if (!read_processing_file_thresholds(counts,
                directory == "collector" ? destination.collector : destination.normalize)) return false;
        }
    }
    return true;
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
    document.reserve(static_cast<std::size_t>(metadata.st_size));
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

    if (cached_settings_ && document == cached_document_) return *cached_settings_;
    auto settings = parse_document(document);
    if (settings) {
        cached_document_ = std::move(document);
        cached_settings_ = *settings;
    }
    return settings;
}

std::expected<MonitorSettings, Error> JsonSettingsReader::parse_document(const std::string& document) const {
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
            return std::unexpected(config_error(path_, "invalid_schema", "Expected schema_version, thresholds, and optional role, process_monitor, processing_files, relay_log, collection, retention and mysql"));
        }
        for (const auto& [key, value] : json.items()) {
            if (key != "schema_version" && key != "thresholds" && key != "collection" && key != "mysql" && key != "is_analyzer" && key != "role" && key != "process_monitor" && key != "processing_files" && key != "relay_log" && key != "retention") {
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
        MonitorSettings settings{policy};
        if (json.contains("retention") && !read_retention(json["retention"], settings.retention)) {
            return std::unexpected(config_error(path_, "invalid_retention",
                "Retention must contain only enabled (boolean) and max_age_seconds (integer between 60 and 31536000)"));
        }
        if (json.contains("is_analyzer")) {
            return std::unexpected(config_error(path_, "invalid_role", "Replace is_analyzer with role col, anal or prx"));
        }
        if (json.contains("role")) {
            const auto& role = json["role"];
            if (!role.is_string() || (role != "col" && role != "anal" && role != "prx")) {
                return std::unexpected(config_error(path_, "invalid_role", "role must be col, anal or prx"));
            }
            settings.role = role == "anal" ? ServerRole::anal
                : role == "prx" ? ServerRole::prx : ServerRole::col;
        }
        if (json.contains("relay_log")) {
            const auto& relay_log = json["relay_log"];
            if (!relay_log.is_object()) {
                return std::unexpected(config_error(path_, "invalid_relay_log", "relay_log must be an object"));
            }
            for (const auto& [key, value] : relay_log.items()) {
                if (key != "path" || !value.is_string()) {
                    return std::unexpected(config_error(path_, "invalid_relay_log",
                        "relay_log supports only a path string"));
                }
                settings.relay_log.path = value.get<std::string>();
            }
        }
        if (json.contains("process_monitor")) {
            const auto& process_monitor = json["process_monitor"];
            if (!process_monitor.is_object()) {
                return std::unexpected(config_error(path_, "invalid_process_monitor", "process_monitor must be an object"));
            }
            for (const auto& [key, value] : process_monitor.items()) {
                if (key != "anal" && key != "col" && key != "prx") {
                    return std::unexpected(config_error(path_, "invalid_process_monitor", "Unknown process_monitor setting"));
                }
                auto& destination = key == "anal" ? settings.process_monitor.anal
                    : key == "prx" ? settings.process_monitor.prx : settings.process_monitor.col;
                if (!read_process_apps(value, destination)) {
                    return std::unexpected(config_error(path_, "invalid_process_monitor", "Process lists must contain at most 256 unique names, each 1..128 UTF-8 bytes without control characters or leading or trailing ASCII whitespace"));
                }
            }
        }
        if (json.contains("processing_files") &&
            !read_processing_files(json["processing_files"], settings.processing_files)) {
            return std::unexpected(config_error(path_, "invalid_processing_files",
                "Processing file thresholds must be objects with collector and normalize integer counts between 0 and 1000000000000"));
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
                    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                        return std::unexpected(config_error(path_, "invalid_interval", "interval_seconds must be an integer between 1 and 86400"));
                    }
                    seconds = static_cast<std::int64_t>(value);
                } else {
                    seconds = interval.get<std::int64_t>();
                }
                settings.collection_interval = std::chrono::seconds{seconds};
            }
            if (collection.contains("output_directory")) {
                const auto& output = collection["output_directory"];
                if (!output.is_string()) {
                    return std::unexpected(config_error(path_, "invalid_output_directory", "output_directory must be an absolute path without control characters"));
                }
                settings.output_directory = output.get<std::string>();
            }
        }
        if (json.contains("mysql")) {
            const auto& mysql = json["mysql"];
            if (!mysql.is_object()) {
                return std::unexpected(config_error(path_, "invalid_mysql", "mysql must be an object"));
            }
            const std::set<std::string> allowed_keys{
                "host", "port", "database", "credentials_file", "connect_timeout_seconds",
                "read_timeout_seconds", "write_timeout_seconds", "tls_mode", "tls_ca_file"
            };
            for (const auto& [key, value] : mysql.items()) {
                if (!allowed_keys.contains(key)) {
                    return std::unexpected(config_error(path_, "invalid_mysql", "Unknown MySQL setting; credentials must be stored in the separate credentials file"));
                }
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
                constexpr auto maximum = std::numeric_limits<unsigned int>::max();
                if (!number.is_number_integer() ||
                    (number.is_number_unsigned() && number.get<std::uint64_t>() > maximum) ||
                    (!number.is_number_unsigned() && (number.get<std::int64_t>() < 0 || number.get<std::int64_t>() > maximum))) {
                    return std::unexpected(config_error(path_, "invalid_mysql", "MySQL port must be 1..65535 and timeouts must be 1..10 integer seconds"));
                }
                *destination = number.get<unsigned int>();
            }
        }
        if (auto valid = validate_settings(settings); !valid) {
            auto error = std::move(valid.error());
            error.target = path_.string();
            return std::unexpected(std::move(error));
        }
        return settings;
    } catch (const nlohmann::json::exception&) {
        return std::unexpected(config_error(path_, "invalid_json", "Invalid settings JSON document"));
    } catch (const std::invalid_argument&) {
        return std::unexpected(config_error(path_, "invalid_json", "Invalid settings JSON structure"));
    }
}

}
