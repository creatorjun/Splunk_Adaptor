// src/application/RelayLogMonitor.hpp
#pragma once

#include "application/ports/IMonitorSettingsProvider.hpp"
#include "application/ports/IRelayLogMonitor.hpp"
#include "application/ports/IRelayLogReader.hpp"

namespace monitor {

class RelayLogMonitor final : public IRelayLogMonitor {
public:
    RelayLogMonitor(IRelayLogReader& reader, const IMonitorSettingsProvider& settings);
    void settings_changed() override;
    RelayLogSnapshot collect() override;
    void acknowledge() noexcept override;

private:
    IRelayLogReader& reader_;
    const IMonitorSettingsProvider& settings_;
};

}
