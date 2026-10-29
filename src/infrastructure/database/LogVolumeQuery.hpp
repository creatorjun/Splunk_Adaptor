// src/infrastructure/database/LogVolumeQuery.hpp
#pragma once

#include "domain/LogVolume.hpp"
#include "domain/LogCountPolicy.hpp"

#include <array>
#include <expected>
#include <map>
#include <set>
#include <string_view>

namespace monitor {

inline constexpr std::size_t log_volume_max_equipment = LogCountPolicy::maximum_equipment;
inline constexpr std::size_t log_volume_max_rows = log_volume_max_equipment * LogCountPolicy::intervals_per_hour;
using LogVolumeQueryRow = std::array<std::optional<std::string_view>, 6>;

std::expected<std::string, Error> make_log_volume_query(const LogVolumeWindow& window);

class LogVolumeQueryResult {
public:
    explicit LogVolumeQueryResult(LogVolumeWindow window);
    std::expected<void, Error> append(const LogVolumeQueryRow& row);
    std::vector<EquipmentLogVolume> take() &&;

private:
    LogVolumeWindow window_;
    std::map<std::uint64_t, std::chrono::system_clock::time_point> intervals_;
    std::set<std::pair<std::string, std::uint64_t>> keys_;
    std::vector<EquipmentLogVolume> counts_;
    std::set<std::string> equipment_;
};

}
