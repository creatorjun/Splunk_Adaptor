// src/application/SettingsController.hpp
#pragma once

#include "application/ports/IOutputDirectory.hpp"
#include "application/ports/IMySqlConfiguration.hpp"
#include "application/ports/ISettingsReader.hpp"
#include "application/ports/IThresholdConfigReader.hpp"
#include "application/ports/IMonitorSettingsProvider.hpp"

#include <optional>

namespace monitor {

struct SettingsOverrides {
    std::optional<std::chrono::seconds> interval;
    std::optional<std::string> output_directory;
};

class SettingsController final : public IThresholdConfigReader, public IMonitorSettingsProvider {
public:
    SettingsController(ISettingsReader& reader, IOutputDirectory& output,
        SettingsOverrides overrides = {}, IMySqlConfiguration* mysql = nullptr,
        IMySqlConfiguration* log_silence_mysql = nullptr,
        IMySqlConfiguration* log_volume_mysql = nullptr);
    std::expected<void, Error> initialize();
    std::expected<bool, Error> reload();
    const MonitorSettings& current() const override;
    std::expected<ThresholdPolicy, Error> read() override;
    std::optional<Error> reload_error() const override;
    std::vector<Error> dependency_errors() const override;

private:
    std::expected<bool, Error> load_and_apply();

    ISettingsReader& reader_;
    IOutputDirectory& output_;
    SettingsOverrides overrides_;
    IMySqlConfiguration* mysql_;
    IMySqlConfiguration* log_silence_mysql_;
    IMySqlConfiguration* log_volume_mysql_;
    std::optional<MonitorSettings> settings_;
    std::optional<Error> last_error_;
    std::vector<Error> dependency_errors_;
};

}
