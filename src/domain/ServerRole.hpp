// src/domain/ServerRole.hpp
#pragma once

#include <string_view>

namespace monitor {

enum class ServerRole { col, anal, prx };

inline std::string_view to_string(ServerRole role) {
    switch (role) {
    case ServerRole::col: return "col";
    case ServerRole::anal: return "anal";
    case ServerRole::prx: return "prx";
    }
    return "unknown";
}

}
