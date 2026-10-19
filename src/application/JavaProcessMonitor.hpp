// src/application/JavaProcessMonitor.hpp
#pragma once

#include "application/ports/IJavaProcessMonitor.hpp"
#include "application/ports/IJavaProcessReader.hpp"
#include "application/ports/IMonitorSettingsProvider.hpp"

namespace monitor {

class JavaProcessMonitor final : public IJavaProcessMonitor {
public:
    JavaProcessMonitor(IJavaProcessReader& reader, const IMonitorSettingsProvider& settings);
    JavaProcessSnapshot collect() override;

private:
    IJavaProcessReader& reader_;
    const IMonitorSettingsProvider& settings_;
};

}
