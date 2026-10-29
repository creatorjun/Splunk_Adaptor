// src/application/LogVolumeMonitor.hpp
#pragma once

#include "application/ports/ILogVolumeCalendar.hpp"
#include "application/ports/ILogVolumeMonitor.hpp"
#include "application/ports/ILogVolumeReader.hpp"
#include "application/ports/IMonitorSettingsProvider.hpp"

namespace monitor {

class LogVolumeMonitor final : public ILogVolumeMonitor {
public:
    LogVolumeMonitor(ILogVolumeReader& reader, const IMonitorSettingsProvider& settings,
        const ILogVolumeCalendar& calendar);
    bool tick(std::chrono::system_clock::time_point timestamp) override;
    LogVolumeAnalysis collect() override;

private:
    ILogVolumeReader& reader_;
    const IMonitorSettingsProvider& settings_;
    const ILogVolumeCalendar& calendar_;
    LogVolumeAnalysis current_;
    std::optional<LogVolumeAnalysis> last_emitted_;
};

}
