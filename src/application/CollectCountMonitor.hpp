// src/application/CollectCountMonitor.hpp
#pragma once

#include "application/ports/ICollectCountMonitor.hpp"
#include "application/ports/ICollectCountReader.hpp"
#include "application/ports/ILocalDayWindow.hpp"
#include "application/ports/IMonitorSettingsProvider.hpp"

namespace monitor {

class CollectCountMonitor final : public ICollectCountMonitor {
public:
    CollectCountMonitor(ICollectCountReader& reader, const IMonitorSettingsProvider& settings,
        const ILocalDayWindow& calendar);
    CollectCountAnalysis collect(std::chrono::system_clock::time_point timestamp) override;
    void settings_changed(std::chrono::system_clock::time_point timestamp) override;

private:
    ICollectCountReader& reader_;
    const IMonitorSettingsProvider& settings_;
    const ILocalDayWindow& calendar_;
};

}
