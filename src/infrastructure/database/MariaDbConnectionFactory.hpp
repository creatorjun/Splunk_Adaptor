// src/infrastructure/database/MariaDbConnectionFactory.hpp
#pragma once

#include "infrastructure/database/IMySqlSession.hpp"

namespace monitor {

class MariaDbConnectionFactory final : public IMySqlConnectionFactory {
public:
    MariaDbConnectionFactory();
    ~MariaDbConnectionFactory() override;
    MariaDbConnectionFactory(const MariaDbConnectionFactory&) = delete;
    MariaDbConnectionFactory& operator=(const MariaDbConnectionFactory&) = delete;

    std::expected<std::unique_ptr<IMySqlSession>, Error> connect(
        const MySqlSettings& settings, const MySqlCredentials& credentials) override;
};

}
