// src/infrastructure/config/JsonSettingsReader.hpp
#pragma once

#include "application/MonitorSettings.hpp"
#include "application/ports/ISettingsReader.hpp"
#include "application/ports/IThresholdConfigReader.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace monitor {

class JsonSettingsReader final : public IThresholdConfigReader, public ISettingsReader {
public:
    explicit JsonSettingsReader(std::filesystem::path path);
    std::expected<ThresholdPolicy, Error> read() override;
    std::expected<MonitorSettings, Error> read_settings() override;

private:
    std::expected<MonitorSettings, Error> parse_document(const std::string& document) const;

    std::filesystem::path path_;
    std::string cached_document_;
    std::optional<MonitorSettings> cached_settings_;
};

}
