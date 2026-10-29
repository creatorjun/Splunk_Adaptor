// src/application/ports/ILogVolumeMonitor.hpp
#pragma once

#include "domain/LogVolume.hpp"

namespace monitor {

class ILogVolumeMonitor {
public:
    virtual ~ILogVolumeMonitor() = default;
    virtual bool tick(std::chrono::system_clock::time_point timestamp) = 0;
    virtual LogVolumeAnalysis collect() = 0;
};

}
