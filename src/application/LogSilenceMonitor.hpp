// src/application/LogSilenceMonitor.hpp
#pragma once

#include "application/ports/ILogSilenceMonitor.hpp"
#include "application/ports/ILogSilenceReader.hpp"
#include "application/ports/IMonitorSettingsProvider.hpp"

namespace monitor {

class LogSilenceMonitor final : public ILogSilenceMonitor {
public:
    LogSilenceMonitor(ILogSilenceReader& reader, const IMonitorSettingsProvider& settings);
    LogSilenceAnalysis collect(std::chrono::system_clock::time_point timestamp) override;
    void settings_changed(std::chrono::system_clock::time_point timestamp) override;

private:
    ILogSilenceReader& reader_;
    const IMonitorSettingsProvider& settings_;
};

}
