// src/application/ports/IMonitorSettingsProvider.hpp
#pragma once

#include "application/MonitorSettings.hpp"

namespace monitor {

class IMonitorSettingsProvider {
public:
    virtual ~IMonitorSettingsProvider() = default;
    virtual const MonitorSettings& current() const = 0;
};

}
