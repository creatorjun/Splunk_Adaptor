// src/application/ports/ILogVolumeCalendar.hpp
#pragma once

#include "application/LogVolumeSettings.hpp"

#include <expected>

namespace monitor {

class ILogVolumeCalendar {
public:
    virtual ~ILogVolumeCalendar() = default;
    virtual std::expected<LogVolumeWindow, Error> window(std::chrono::system_clock::time_point timestamp,
        const LogVolumeSettings& settings) const = 0;
};

}
