// src/application/ports/IRelayLogMonitor.hpp
#pragma once

#include "domain/RelayLog.hpp"

namespace monitor {

class IRelayLogMonitor {
public:
    virtual ~IRelayLogMonitor() = default;
    virtual void settings_changed() = 0;
    virtual RelayLogSnapshot collect() = 0;
    virtual void acknowledge() noexcept = 0;
};

}
