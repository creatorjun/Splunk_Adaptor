// src/application/ports/IProcessingFileMonitor.hpp
#pragma once

#include "domain/ProcessingFilesSnapshot.hpp"

namespace monitor {

class IProcessingFileMonitor {
public:
    virtual ~IProcessingFileMonitor() = default;
    virtual ProcessingFilesSnapshot collect() = 0;
    virtual void settings_changed() {}
};

}
