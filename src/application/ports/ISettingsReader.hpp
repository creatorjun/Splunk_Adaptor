// src/application/ports/ISettingsReader.hpp
#pragma once

#include "application/MonitorSettings.hpp"

#include <expected>

namespace monitor {

class ISettingsReader {
public:
    virtual ~ISettingsReader() = default;
    virtual std::expected<MonitorSettings, Error> read_settings() = 0;
};

}
