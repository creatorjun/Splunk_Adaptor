// src/infrastructure/config/JsonSettingsReader.hpp
#pragma once

#include "application/MonitorSettings.hpp"
#include "application/ports/IThresholdConfigReader.hpp"

#include <filesystem>

namespace monitor {

class JsonSettingsReader final : public IThresholdConfigReader {
public:
    explicit JsonSettingsReader(std::filesystem::path path);
    std::expected<ThresholdPolicy, Error> read() override;
    std::expected<MonitorSettings, Error> read_settings();

private:
    std::filesystem::path path_;
};

}
