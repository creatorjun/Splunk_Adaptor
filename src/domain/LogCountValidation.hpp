// src/domain/LogCountValidation.hpp
#pragma once

#include "domain/LogVolume.hpp"

namespace monitor {

bool valid_log_count_boundary(std::chrono::system_clock::time_point timestamp) noexcept;
bool valid_log_count_equipment(const std::vector<EquipmentLogVolume>& equipment);
bool valid_log_count_hour(const LogCountHour& hour, bool completed);
bool valid_log_count_state(const LogCountState& state);

}
