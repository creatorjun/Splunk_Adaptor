// src/application/CheckSettings.cpp
#include "application/CheckSettings.hpp"
#include "application/SettingsValidation.hpp"

namespace monitor {

std::expected<void, Error> check_settings(ISettingsReader& settings, IMySqlCredentialsReader& credentials) {
    const auto parsed = settings.read_settings();
    if (!parsed) return std::unexpected(parsed.error());
    if (const auto valid = validate_settings(*parsed); !valid) return valid;
    if (parsed->role == ServerRole::anal) {
        const auto secret = credentials.read_credentials(parsed->mysql.credentials_file);
        if (!secret) return std::unexpected(secret.error());
    }
    return {};
}

}
