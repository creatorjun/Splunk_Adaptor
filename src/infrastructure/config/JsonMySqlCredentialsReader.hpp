// src/infrastructure/config/JsonMySqlCredentialsReader.hpp
#pragma once

#include "application/ports/IMySqlCredentialsReader.hpp"

namespace monitor {

class JsonMySqlCredentialsReader final : public IMySqlCredentialsReader {
public:
    std::expected<MySqlCredentials, Error> read_credentials(const std::string& path) override;
};

}
