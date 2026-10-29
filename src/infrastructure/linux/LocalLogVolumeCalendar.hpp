// src/infrastructure/linux/LocalLogVolumeCalendar.hpp
#pragma once

#include "application/ports/ILogVolumeCalendar.hpp"

namespace monitor {

class LocalLogVolumeCalendar final : public ILogVolumeCalendar {
public:
    std::expected<LogVolumeWindow, Error> window(std::chrono::system_clock::time_point timestamp) const override;
    std::expected<LogVolumeWindow, Error> range(std::chrono::system_clock::time_point start,
        std::chrono::system_clock::time_point end) const override;
    std::expected<LogCountHour, Error> hour(std::chrono::system_clock::time_point timestamp) const override;
};

}
