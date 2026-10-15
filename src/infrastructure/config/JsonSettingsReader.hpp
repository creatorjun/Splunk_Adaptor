// src/infrastructure/config/JsonSettingsReader.hpp
#pragma once

#include "application/MonitorSettings.hpp"
#include "application/ports/ISettingsReader.hpp"
#include "application/ports/IThresholdConfigReader.hpp"

#include <filesystem>

namespace monitor {

class JsonSettingsReader final : public IThresholdConfigReader, public ISettingsReader {
public:
    explicit JsonSettingsReader(std::filesystem::path path);
    std::expected<ThresholdPolicy, Error> read() override;
    std::expected<MonitorSettings, Error> read_settings() override;

private:
    std::filesystem::path path_;
};

}
