// src/application/CheckSettings.hpp
#pragma once

#include "application/ports/IMySqlCredentialsReader.hpp"
#include "application/ports/ISettingsReader.hpp"

namespace monitor {

std::expected<void, Error> check_settings(ISettingsReader& settings, IMySqlCredentialsReader& credentials);

}
