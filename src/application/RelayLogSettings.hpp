// src/application/RelayLogSettings.hpp
#pragma once

#include <string>

namespace monitor {

struct RelayLogSettings {
    std::string path{"/CloudESM/logs/CollectorForAgent.log"};

    bool operator==(const RelayLogSettings&) const = default;
};

}
