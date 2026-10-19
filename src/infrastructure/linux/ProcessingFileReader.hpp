// src/infrastructure/linux/ProcessingFileReader.hpp
#pragma once

#include "application/ports/IProcessingFileReader.hpp"
#include "application/ports/IProcessingFileScanner.hpp"

#include <memory>

namespace monitor {

class ProcessingFileReader final : public IProcessingFileReader {
public:
    explicit ProcessingFileReader(IProcessingFileScanner& scanner,
        std::chrono::milliseconds interval = std::chrono::seconds{60});
    ~ProcessingFileReader() override;
    ProcessingFileReader(const ProcessingFileReader&) = delete;
    ProcessingFileReader& operator=(const ProcessingFileReader&) = delete;
    ProcessingFilesSnapshot read(bool enabled) override;

private:
    class State;
    std::unique_ptr<State> state_;
};

}
