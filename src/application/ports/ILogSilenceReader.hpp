// src/application/ports/ILogSilenceReader.hpp
#pragma once

#include "domain/LogSilenceAnalysis.hpp"

namespace monitor {

class ILogSilenceReader {
public:
    virtual ~ILogSilenceReader() = default;
    virtual LogSilenceReading read_last_received(bool enabled,
        std::chrono::seconds refresh_interval) = 0;
};

}
