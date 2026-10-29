// src/domain/LogVolume.hpp
#pragma once

#include "domain/ResourceSample.hpp"

namespace monitor {

enum class LogVolumeMode { interval, daily };

struct LogVolumeWindow {
    LogVolumeMode mode{LogVolumeMode::interval};
    std::uint64_t start_bucket{};
    std::uint64_t end_bucket{};
    std::chrono::system_clock::time_point start;
    std::chrono::system_clock::time_point end;

    bool operator==(const LogVolumeWindow&) const = default;
};

struct EquipmentLogVolume {
    std::string eqp_ip;
    std::uint64_t count{};

    bool operator==(const EquipmentLogVolume&) const = default;
};

enum class LogVolumeReadStatus { pending, ready, error };

struct LogVolumeReading {
    LogVolumeReadStatus status{LogVolumeReadStatus::pending};
    std::optional<LogVolumeWindow> window;
    std::optional<std::chrono::system_clock::time_point> queried_at;
    std::vector<EquipmentLogVolume> equipment;
    std::optional<Error> error;
};

enum class LogVolumeStatus { disabled, pending, ok, no_data, error };

struct LogVolumeAnalysis {
    LogVolumeStatus status{LogVolumeStatus::disabled};
    LogVolumeMode mode{LogVolumeMode::interval};
    std::optional<LogVolumeWindow> window;
    std::optional<std::chrono::system_clock::time_point> queried_at;
    std::vector<EquipmentLogVolume> equipment;
    std::optional<Error> error;
};

const char* to_string(LogVolumeMode mode) noexcept;
const char* to_string(LogVolumeStatus status) noexcept;

}
