// src/application/MonitorSettings.hpp
#pragma once

#include "application/MySqlSettings.hpp"
#include "application/ProcessMonitorSettings.hpp"
#include "application/ProcessingFileSettings.hpp"
#include "application/SnapshotRetentionSettings.hpp"
#include "application/RelayLogSettings.hpp"
#include "domain/ServerRole.hpp"
#include "domain/ThresholdPolicy.hpp"

#include <chrono>
#include <string>

namespace monitor {

struct MonitorSettings {
    ThresholdPolicy thresholds;
    std::chrono::seconds collection_interval{60};
    std::string output_directory{"/Monitor"};
    MySqlSettings mysql{};
    ServerRole role{ServerRole::col};
    ProcessMonitorSettings process_monitor{};
    ProcessingFileSettings processing_files{};
    SnapshotRetentionSettings retention{};
    RelayLogSettings relay_log{};

    bool operator==(const MonitorSettings&) const = default;
};

}
