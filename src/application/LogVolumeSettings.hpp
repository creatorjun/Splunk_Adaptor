// src/application/LogVolumeSettings.hpp
#pragma once

#include "domain/LogVolume.hpp"

namespace monitor {

struct LogVolumeSettings {
    LogVolumeMode mode{LogVolumeMode::interval};
    unsigned int interval_seconds{300};
    unsigned int daily_at_minutes{10};

    bool operator==(const LogVolumeSettings&) const = default;
};

}
