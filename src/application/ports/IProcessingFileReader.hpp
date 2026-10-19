// src/application/ports/IProcessingFileReader.hpp
#pragma once

#include "domain/ProcessingFilesSnapshot.hpp"

namespace monitor {

class IProcessingFileReader {
public:
    virtual ~IProcessingFileReader() = default;
    virtual ProcessingFilesSnapshot read(bool enabled) = 0;
};

}
