// src/application/ports/ILogVolumeCalendar.hpp
#pragma once

#include "domain/LogVolume.hpp"

#include <expected>

namespace monitor {

class ILogVolumeCalendar {
public:
    virtual ~ILogVolumeCalendar() = default;
    virtual std::expected<LogVolumeWindow, Error> window(std::chrono::system_clock::time_point timestamp) const = 0;
    virtual std::expected<LogVolumeWindow, Error> range(std::chrono::system_clock::time_point start,
        std::chrono::system_clock::time_point end) const = 0;
    virtual std::expected<LogCountHour, Error> hour(std::chrono::system_clock::time_point timestamp) const = 0;
};

}
