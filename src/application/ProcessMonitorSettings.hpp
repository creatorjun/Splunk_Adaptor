// src/application/ProcessMonitorSettings.hpp
#pragma once

#include <string>
#include <vector>

namespace monitor {

struct ProcessMonitorSettings {
    std::vector<std::string> anal{
        "SearchEngine", "Normalizer", "CollectorForAgent", "CDMEngine", "ShovelServer",
        "AnalyzeEngine", "AnalyzeAssistantEngine", "SoarTicketMain", "SoarPlaybookManager",
        "pmsServer", "pmsClient", "ProcessMonitor"};
    std::vector<std::string> col{
        "SearchEngine", "Reducer", "Normalizer", "CloudESMCollector", "CollectorForAgent",
        "CDMEngine", "pmsClient", "ProcessMonitor"};
    std::vector<std::string> prx{"CollectorForAgent"};

    bool operator==(const ProcessMonitorSettings&) const = default;
};

}
