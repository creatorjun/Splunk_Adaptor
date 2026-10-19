// src/application/ProcessingFileMonitor.hpp
#pragma once

#include "application/ports/IMonitorSettingsProvider.hpp"
#include "application/ports/IProcessingFileMonitor.hpp"
#include "application/ports/IProcessingFileReader.hpp"

namespace monitor {

class ProcessingFileMonitor final : public IProcessingFileMonitor {
public:
    ProcessingFileMonitor(IProcessingFileReader& reader, const IMonitorSettingsProvider& settings);
    ProcessingFilesSnapshot collect() override;
    void settings_changed() override;

private:
    IProcessingFileReader& reader_;
    const IMonitorSettingsProvider& settings_;
};

}
