// src/application/ProcessMonitorSettings.hpp
#pragma once

#include <string>
#include <vector>

namespace monitor {

struct ProcessMonitorSettings {
    std::vector<std::string> analyzer{
        "SearchEngine", "Normalizer", "CollectorForAgent", "CDMEngine", "ShovelServer",
        "AnalyzeEngine", "AnalyzeAssistantEngine", "SoarTicketMain", "SoarPlaybookManager",
        "pmsServer", "pmsClient", "ProcessMonitor"};
    std::vector<std::string> collector{
        "SearchEngine", "Reducer", "Normalizer", "CloudESMCollector", "CollectorForAgent",
        "CDMEngine", "pmsClient", "ProcessMonitor"};
    std::vector<std::string> relay{"CollectorForAgent"};

    bool operator==(const ProcessMonitorSettings&) const = default;
};

}
