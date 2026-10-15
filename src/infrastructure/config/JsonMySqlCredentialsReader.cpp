// src/infrastructure/config/JsonMySqlCredentialsReader.cpp
#include "infrastructure/config/JsonMySqlCredentialsReader.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace monitor {
namespace {

class CredentialsFd {
public:
    explicit CredentialsFd(int value) noexcept : value_(value) {}
    ~CredentialsFd() { if (value_ >= 0) ::close(value_); }
    CredentialsFd(const CredentialsFd&) = delete;
    CredentialsFd& operator=(const CredentialsFd&) = delete;
    int get() const noexcept { return value_; }

private:
    int value_;
};

Error credentials_error(std::string code, std::string message) {
    return {"mysql_credentials", "mysql", std::move(code), std::move(message)};
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

std::expected<MySqlCredentials, Error> JsonMySqlCredentialsReader::read_credentials(const std::string& path) {
    constexpr std::size_t maximum_bytes = 16 * 1024;
    if (path.empty() || path.front() != '/' || has_control_characters(path)) {
        return std::unexpected(credentials_error("invalid_path", "Credentials file requires an absolute path without control characters"));
    }
    const CredentialsFd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW));
    if (file.get() < 0) {
        return std::unexpected(credentials_error("io_error", "Cannot open credentials file"));
    }
    struct stat metadata {};
    if (::fstat(file.get(), &metadata) < 0) {
        return std::unexpected(credentials_error("io_error", "Cannot inspect credentials file"));
    }
    if (!S_ISREG(metadata.st_mode)) {
        return std::unexpected(credentials_error("invalid_file", "Credentials must be a regular file"));
    }
    const auto permissions = metadata.st_mode & 07777;
    if ((permissions != 0600 && permissions != 0640) ||
        (metadata.st_uid != 0 && metadata.st_uid != ::geteuid())) {
        return std::unexpected(credentials_error("insecure_permissions", "Credentials file must be owned by root or the daemon user with permissions 0600 or 0640"));
    }
    if (metadata.st_size < 0 || static_cast<std::uintmax_t>(metadata.st_size) > maximum_bytes) {
        return std::unexpected(credentials_error("file_too_large", "Credentials file exceeds 16384 bytes"));
    }
    std::string document;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(credentials_error("io_error", "Cannot read credentials file"));
        }
        if (count == 0) break;
        const auto amount = static_cast<std::size_t>(count);
        if (amount > maximum_bytes - document.size()) {
            return std::unexpected(credentials_error("file_too_large", "Credentials file exceeds 16384 bytes"));
        }
        document.append(buffer.data(), amount);
    }
    try {
        std::vector<std::set<std::string>> object_keys;
        const auto callback = [&object_keys](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value) {
            if (depth > 4) throw std::invalid_argument("Invalid credentials nesting");
            if (event == nlohmann::json::parse_event_t::object_start) {
                object_keys.emplace_back();
            } else if (event == nlohmann::json::parse_event_t::object_end) {
                object_keys.pop_back();
            } else if (event == nlohmann::json::parse_event_t::key) {
                if (!object_keys.back().insert(value.get<std::string>()).second) {
                    throw std::invalid_argument("Duplicate credentials key");
                }
            }
            return true;
        };
        const auto json = nlohmann::json::parse(document, callback);
        if (!json.is_object() || json.size() != 2 || !json.contains("username") || !json.contains("password") ||
            !json["username"].is_string() || !json["password"].is_string()) {
            return std::unexpected(credentials_error("invalid_schema", "Credentials require exactly username and password strings"));
        }
        MySqlCredentials credentials{json["username"].get<std::string>(), json["password"].get<std::string>()};
        if (credentials.username.empty() || credentials.username.size() > 128 || has_control_characters(credentials.username)) {
            return std::unexpected(credentials_error("invalid_username", "Username must contain between 1 and 128 bytes without control characters"));
        }
        if (credentials.password.empty() || credentials.password.size() > 4096 || credentials.password.find('\0') != std::string::npos) {
            return std::unexpected(credentials_error("invalid_password", "Password must contain between 1 and 4096 bytes without NUL characters"));
        }
        return credentials;
    } catch (const nlohmann::json::exception&) {
        return std::unexpected(credentials_error("invalid_json", "Invalid credentials JSON document"));
    } catch (const std::invalid_argument&) {
        return std::unexpected(credentials_error("invalid_json", "Invalid credentials JSON structure"));
    }
}

}
