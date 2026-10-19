// src/application/ports/ILogSilenceMonitor.hpp
#pragma once

#include "domain/LogSilenceAnalysis.hpp"

namespace monitor {

class ILogSilenceMonitor {
public:
    virtual ~ILogSilenceMonitor() = default;
    virtual LogSilenceAnalysis collect(std::chrono::system_clock::time_point timestamp) = 0;
    virtual void settings_changed(std::chrono::system_clock::time_point timestamp) = 0;
};

}
