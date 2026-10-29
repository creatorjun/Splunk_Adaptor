// src/domain/LogVolume.hpp
#pragma once

#include "domain/ResourceSample.hpp"

namespace monitor {

struct LogVolumeWindow {
    std::chrono::system_clock::time_point start;
    std::chrono::system_clock::time_point end;

    bool operator==(const LogVolumeWindow&) const = default;
};

struct EquipmentLogVolume {
    std::string eqp_ip;
    std::uint64_t count{};
    std::chrono::system_clock::time_point interval_start;

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
    std::optional<LogVolumeWindow> window;
    std::optional<std::chrono::system_clock::time_point> queried_at;
    std::optional<std::chrono::system_clock::time_point> last_saved_hour;
    unsigned int completed_intervals{};
    bool backlog{false};
    std::optional<Error> error;
};

struct LogCountHour {
    std::chrono::system_clock::time_point start;
    std::chrono::system_clock::time_point end;
    std::chrono::system_clock::time_point coverage_start;
    unsigned int completed_intervals{};
    std::vector<EquipmentLogVolume> equipment;

    bool operator==(const LogCountHour&) const = default;
};

struct LogCountState {
    std::string source_identity;
    std::chrono::system_clock::time_point next_start;
    std::optional<LogCountHour> hour;
    std::optional<std::chrono::system_clock::time_point> last_saved_hour;

    bool operator==(const LogCountState&) const = default;
};

struct LogCountReport {
    std::string source_identity;
    LogCountHour hour;
    std::chrono::system_clock::time_point collected_at;
};

const char* to_string(LogVolumeStatus status) noexcept;

}