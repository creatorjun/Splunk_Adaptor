// src/application/MonitorSettings.hpp
#pragma once

#include "application/MySqlSettings.hpp"
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

    bool mysql_enabled() const noexcept { return is_analyzer && mysql.enabled; }

    bool operator==(const MonitorSettings&) const = default;
};

}
