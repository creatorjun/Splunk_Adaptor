// src/application/ports/ICollectCountMonitor.hpp
#pragma once

#include "domain/CollectCountAnalysis.hpp"

namespace monitor {

class ICollectCountMonitor {
public:
    virtual ~ICollectCountMonitor() = default;
    virtual CollectCountAnalysis collect(std::chrono::system_clock::time_point timestamp) = 0;
    virtual void settings_changed(std::chrono::system_clock::time_point timestamp) = 0;
};

}
