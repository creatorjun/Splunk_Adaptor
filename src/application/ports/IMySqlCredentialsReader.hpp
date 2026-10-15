// src/application/ports/IMySqlCredentialsReader.hpp
#pragma once

#include "domain/ResourceSample.hpp"

#include <expected>
#include <string>

namespace monitor {

struct MySqlCredentials {
    std::string username;
    std::string password;

    bool operator==(const MySqlCredentials&) const = default;
};

class IMySqlCredentialsReader {
public:
    virtual ~IMySqlCredentialsReader() = default;
    virtual std::expected<MySqlCredentials, Error> read_credentials(const std::string& path) = 0;
};

}
