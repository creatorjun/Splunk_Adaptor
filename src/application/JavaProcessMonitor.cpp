// src/application/JavaProcessMonitor.cpp
#include "application/JavaProcessMonitor.hpp"

#include <array>
#include <span>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace monitor {
namespace {

constexpr std::array<std::string_view, 12> analyzer_apps{
    "SearchEngine", "Normalizer", "CollectorForAgent", "CDMEngine", "ShovelServer",
    "AnalyzeEngine", "AnalyzeAssistantEngine", "SoarTicketMain", "SoarPlaybookManager",
    "pmsServer", "pmsClient", "ProcessMonitor"};

constexpr std::array<std::string_view, 8> collector_apps{
    "SearchEngine", "Reducer", "Normalizer", "CloudESMCollector", "CollectorForAgent",
    "CDMEngine", "pmsClient", "ProcessMonitor"};

}

JavaProcessMonitor::JavaProcessMonitor(IJavaProcessReader& reader,
    const IMonitorSettingsProvider& settings) : reader_(reader), settings_(settings) {
}

JavaProcessSnapshot JavaProcessMonitor::collect() {
    JavaProcessSnapshot result;
    result.is_analyzer = settings_.current().is_analyzer;
    auto reading = reader_.read();
    const std::unordered_set<std::string_view> running{
        reading.running_apps.begin(), reading.running_apps.end()};
    const auto expected = result.is_analyzer ? std::span<const std::string_view>{analyzer_apps}
        : std::span<const std::string_view>{collector_apps};
    result.status = reading.errors.empty() ? MetricStatus::ok : MetricStatus::unknown;
    for (const auto app : expected) {
        std::optional<bool> active;
        if (running.contains(app)) {
            active = true;
        } else if (reading.errors.empty()) {
            active = false;
            result.status = MetricStatus::warning;
        }
        result.apps.emplace(app, active);
    }
    result.errors = std::move(reading.errors);
    return result;
}

}
