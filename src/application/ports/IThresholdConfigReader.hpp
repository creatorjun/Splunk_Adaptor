// src/application/ports/IThresholdConfigReader.hpp
#pragma once

#include "domain/ThresholdPolicy.hpp"

#include <expected>

namespace monitor {

class IThresholdConfigReader {
public:
    virtual ~IThresholdConfigReader() = default;
    virtual std::expected<ThresholdPolicy, Error> read() = 0;
};

}
