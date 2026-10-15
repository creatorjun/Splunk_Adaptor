// src/application/MySqlSettings.hpp
#pragma once

#include <string>

namespace monitor {

struct MySqlSettings {
    bool enabled{false};
    std::string host{"127.0.0.1"};
    unsigned int port{3306};
    std::string database;
    std::string credentials_file{"/etc/resource-monitor/secrets/mysql.json"};
    unsigned int connect_timeout_seconds{3};
    unsigned int read_timeout_seconds{3};
    unsigned int write_timeout_seconds{3};
    std::string tls_mode{"verify_identity"};
    std::string tls_ca_file;

    bool operator==(const MySqlSettings&) const = default;
};

}
