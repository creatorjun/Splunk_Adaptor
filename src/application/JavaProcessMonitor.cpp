// src/application/JavaProcessMonitor.cpp
#include "application/JavaProcessMonitor.hpp"

#include <string_view>
#include <unordered_set>
#include <utility>

namespace monitor {
JavaProcessMonitor::JavaProcessMonitor(IJavaProcessReader& reader,
    const IMonitorSettingsProvider& settings) : reader_(reader), settings_(settings) {
}

JavaProcessSnapshot JavaProcessMonitor::collect() {
    JavaProcessSnapshot result;
    const auto& settings = settings_.current();
    result.role = settings.role;
    const auto& expected = settings.role == ServerRole::analyzer ? settings.process_monitor.analyzer
        : settings.role == ServerRole::relay ? settings.process_monitor.relay : settings.process_monitor.collector;
    if (expected.empty()) {
        result.status = MetricStatus::ok;
        return result;
    }
    auto reading = reader_.read();
    const std::unordered_set<std::string_view> running{
        reading.running_apps.begin(), reading.running_apps.end()};
    result.status = reading.errors.empty() ? MetricStatus::ok : MetricStatus::unknown;
    for (const auto& app : expected) {
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
