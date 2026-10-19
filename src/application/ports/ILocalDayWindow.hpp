// src/application/ports/ILocalDayWindow.hpp
#pragma once

#include "application/AnalyzerSettings.hpp"
#include "domain/CollectCountAnalysis.hpp"

namespace monitor {

class ILocalDayWindow {
public:
    virtual ~ILocalDayWindow() = default;
    virtual std::expected<DailyCountWindow, Error> make(
        std::chrono::system_clock::time_point timestamp, ComparisonDay day) const = 0;
};

}
