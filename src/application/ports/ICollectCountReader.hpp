// src/application/ports/ICollectCountReader.hpp
#pragma once

#include "domain/CollectCountAnalysis.hpp"

namespace monitor {

class ICollectCountReader {
public:
    virtual ~ICollectCountReader() = default;
    virtual CollectCountReading read_daily_counts(const DailyCountWindow& window,
        std::chrono::seconds refresh_interval) = 0;
};

}
