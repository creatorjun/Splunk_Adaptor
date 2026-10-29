// src/application/RelayLogMonitor.cpp
#include "application/RelayLogMonitor.hpp"

namespace monitor {

RelayLogMonitor::RelayLogMonitor(IRelayLogReader& reader, const IMonitorSettingsProvider& settings)
    : reader_(reader), settings_(settings) {}

void RelayLogMonitor::settings_changed() {
    collect();
}

RelayLogSnapshot RelayLogMonitor::collect() {
    const auto& settings = settings_.current();
    return reader_.read(settings.role == ServerRole::relay, settings.relay_log.path);
}

void RelayLogMonitor::acknowledge() noexcept {
    reader_.acknowledge();
}

}
