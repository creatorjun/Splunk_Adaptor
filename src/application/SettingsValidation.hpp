// src/application/SettingsValidation.hpp
#pragma once

#include "application/MonitorSettings.hpp"

#include <expected>

namespace monitor {

std::expected<void, Error> validate_settings(const MonitorSettings& settings);

}
