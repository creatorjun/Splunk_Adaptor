// src/domain/ServerRole.hpp
#pragma once

#include <string_view>

namespace monitor {

enum class ServerRole { collector, analyzer, relay };

inline std::string_view to_string(ServerRole role) {
    switch (role) {
    case ServerRole::collector: return "collector";
    case ServerRole::analyzer: return "analyzer";
    case ServerRole::relay: return "relay";
    }
    return "unknown";
}

}
