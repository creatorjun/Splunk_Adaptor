// src/infrastructure/database/IMySqlSession.hpp
#pragma once

#include "application/MySqlSettings.hpp"
#include "application/ports/IMySqlCredentialsReader.hpp"
#include "domain/LogVolume.hpp"

#include <expected>
#include <memory>

namespace monitor {

class IMySqlSession {
public:
    virtual ~IMySqlSession() = default;
    virtual std::expected<void, Error> probe() = 0;
    virtual std::expected<std::vector<EquipmentLogVolume>, Error> read_log_volume(const LogVolumeWindow&) {
        return std::unexpected(Error{"log_volume", "query", "unsupported",
            "Windowed log volume queries are unavailable"});
    }
};

class IMySqlConnectionFactory {
public:
    virtual ~IMySqlConnectionFactory() = default;
    virtual std::expected<std::unique_ptr<IMySqlSession>, Error> connect(
        const MySqlSettings& settings, const MySqlCredentials& credentials) = 0;
};

}
