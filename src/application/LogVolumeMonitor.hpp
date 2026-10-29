// src/application/LogVolumeMonitor.hpp
#pragma once

#include "application/ports/ILogCountStore.hpp"
#include "application/ports/ILogVolumeCalendar.hpp"
#include "application/ports/ILogVolumeMonitor.hpp"
#include "application/ports/ILogVolumeReader.hpp"
#include "application/ports/IMonitorSettingsProvider.hpp"

namespace monitor {

class LogVolumeMonitor final : public ILogVolumeMonitor {
public:
    LogVolumeMonitor(ILogVolumeReader& reader, const IMonitorSettingsProvider& settings,
        const ILogVolumeCalendar& calendar, ILogCountStore& store);
    bool tick(std::chrono::system_clock::time_point timestamp) override;
    LogVolumeAnalysis collect() override;

private:
    struct PendingCommit {
        LogCountState state;
        std::vector<LogCountReport> reports;
        LogVolumeAnalysis analysis;
    };

    std::expected<PendingCommit, Error> aggregate(const LogVolumeReading& reading,
        std::chrono::system_clock::time_point timestamp) const;
    bool publish(LogVolumeAnalysis next);
    bool fail(Error error);
    ILogVolumeReader& reader_;
    const IMonitorSettingsProvider& settings_;
    const ILogVolumeCalendar& calendar_;
    ILogCountStore& store_;
    LogVolumeAnalysis current_;
    std::optional<LogVolumeAnalysis> last_emitted_;
    std::optional<LogCountState> state_;
    std::optional<LogVolumeWindow> active_;
    std::optional<PendingCommit> pending_;
    std::optional<std::chrono::system_clock::time_point> next_attempt_;
    std::optional<std::chrono::system_clock::time_point> next_prune_;
    std::optional<Error> retention_error_;
    LogVolumeStatus status_before_retention_error_{LogVolumeStatus::pending};
    std::string directory_;
};

}
