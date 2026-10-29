// src/domain/LogVolume.cpp
#include "domain/LogVolume.hpp"

namespace monitor {

const char* to_string(LogVolumeStatus status) noexcept {
    switch (status) {
    case LogVolumeStatus::disabled: return "disabled";
    case LogVolumeStatus::pending: return "pending";
    case LogVolumeStatus::ok: return "ok";
    case LogVolumeStatus::no_data: return "no_data";
    case LogVolumeStatus::error: return "error";
    }
    return "error";
}

}