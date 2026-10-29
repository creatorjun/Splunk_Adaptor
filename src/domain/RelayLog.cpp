// src/domain/RelayLog.cpp
#include "domain/RelayLog.hpp"

namespace monitor {

const char* to_string(RelayLogStatus status) noexcept {
    switch (status) {
    case RelayLogStatus::disabled: return "disabled";
    case RelayLogStatus::ok: return "ok";
    case RelayLogStatus::warning: return "warning";
    case RelayLogStatus::error: return "error";
    case RelayLogStatus::partial: return "partial";
    }
    return "error";
}

}
