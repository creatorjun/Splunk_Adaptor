// src/infrastructure/config/JsonThresholdConfigReader.cpp
#include "infrastructure/config/JsonThresholdConfigReader.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <fcntl.h>
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
    return {"threshold_config", path.string(), std::move(code), std::move(message)};
}

Error io_error(const std::filesystem::path& path, const char* operation, int error) {
    return config_error(path, "io_error", std::string(operation) + ": " +
        std::error_code(error, std::generic_category()).message());
}

}

JsonThresholdConfigReader::JsonThresholdConfigReader(std::filesystem::path path)
    : path_(std::move(path)) {}

std::expected<ThresholdPolicy, Error> JsonThresholdConfigReader::read() {
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
        if (!json.is_object() || json.size() != 2 || !json.contains("schema_version") ||
            !json.contains("thresholds")) {
            return std::unexpected(config_error(path_, "invalid_schema", "Expected only schema_version and thresholds"));
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
            error.component = "threshold_config";
            error.target = path_.string();
            return std::unexpected(std::move(error));
        }
        return policy;
    } catch (const nlohmann::json::exception& error) {
        return std::unexpected(config_error(path_, "invalid_json", error.what()));
    } catch (const std::invalid_argument& error) {
        return std::unexpected(config_error(path_, "invalid_json", error.what()));
    }
}

}
