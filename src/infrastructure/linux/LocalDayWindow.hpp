// src/infrastructure/linux/LocalDayWindow.hpp
#pragma once

#include "application/ports/ILocalDayWindow.hpp"

namespace monitor {

class LocalDayWindow final : public ILocalDayWindow {
public:
    std::expected<DailyCountWindow, Error> make(std::chrono::system_clock::time_point timestamp,
        ComparisonDay day) const override;
};

}
