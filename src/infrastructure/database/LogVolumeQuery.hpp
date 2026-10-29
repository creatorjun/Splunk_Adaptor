// src/infrastructure/database/LogVolumeQuery.hpp
#pragma once

#include "domain/LogVolume.hpp"

#include <array>
#include <expected>
#include <set>
#include <string_view>

namespace monitor {

inline constexpr std::size_t log_volume_max_equipment = 10000;
inline constexpr std::size_t log_volume_max_rows = log_volume_max_equipment * 12;
using LogVolumeQueryRow = std::array<std::optional<std::string_view>, 6>;

std::expected<std::string, Error> make_log_volume_query(const LogVolumeWindow& window);

class LogVolumeQueryResult {
public:
    explicit LogVolumeQueryResult(LogVolumeWindow window) : window_(window) {}
    std::expected<void, Error> append(const LogVolumeQueryRow& row);
    std::vector<EquipmentLogVolume> take() &&;

private:
    LogVolumeWindow window_;
    std::set<std::pair<std::string, std::uint64_t>> keys_;
    std::vector<EquipmentLogVolume> counts_;
    std::set<std::string> equipment_;
};

}
