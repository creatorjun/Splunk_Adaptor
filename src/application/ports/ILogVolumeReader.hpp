// src/application/ports/ILogVolumeReader.hpp
#pragma once

#include "domain/LogVolume.hpp"

namespace monitor {

class ILogVolumeReader {
public:
    virtual ~ILogVolumeReader() = default;
    virtual LogVolumeReading read_log_volume(const LogVolumeWindow& window) = 0;
};

}
