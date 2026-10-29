// src/infrastructure/linux/LocalLogVolumeCalendar.hpp
#pragma once

#include "application/ports/ILogVolumeCalendar.hpp"

namespace monitor {

class LocalLogVolumeCalendar final : public ILogVolumeCalendar {
public:
    std::expected<LogVolumeWindow, Error> window(std::chrono::system_clock::time_point timestamp,
        const LogVolumeSettings& settings) const override;
};

}
