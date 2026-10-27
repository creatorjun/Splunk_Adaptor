// src/application/MonitorSettings.hpp
#pragma once

#include "application/MySqlSettings.hpp"
#include "application/AnalyzerSettings.hpp"
#include "application/ProcessMonitorSettings.hpp"
#include "application/LogSilenceSettings.hpp"
#include "application/ProcessingFileSettings.hpp"
#include "application/SnapshotRetentionSettings.hpp"
#include "domain/ThresholdPolicy.hpp"

#include <chrono>
#include <string>

namespace monitor {

struct MonitorSettings {
    ThresholdPolicy thresholds;
    std::chrono::seconds collection_interval{60};
    std::string output_directory{"/Monitor"};
    MySqlSettings mysql{};
    bool is_analyzer{false};
    AnalyzerSettings analyzer{};
    ProcessMonitorSettings process_monitor{};
    LogSilenceSettings log_silence{};
    ProcessingFileSettings processing_files{};
    SnapshotRetentionSettings retention{};

    bool operator==(const MonitorSettings&) const = default;
};

}
