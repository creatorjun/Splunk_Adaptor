// src/application/LogSilenceSettings.hpp
#pragma once

#include "domain/LogSilenceAnalysis.hpp"

#include <vector>

namespace monitor {

struct LogSilenceSettings {
    std::uint32_t default_timeout_seconds{300};
    std::vector<LogSilencePolicy> overrides;

    bool operator==(const LogSilenceSettings&) const = default;
};

}
