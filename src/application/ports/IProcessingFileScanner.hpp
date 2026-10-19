// src/application/ports/IProcessingFileScanner.hpp
#pragma once

#include "domain/ProcessingFilesSnapshot.hpp"

#include <stop_token>

namespace monitor {

class IProcessingFileScanner {
public:
    virtual ~IProcessingFileScanner() = default;
    virtual ProcessingFilesSnapshot scan(std::stop_token stop) = 0;
};

}
