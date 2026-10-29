// src/application/LogCountPolicy.hpp
#pragma once

#include <chrono>

namespace monitor {

struct LogCountPolicy {
    static constexpr std::chrono::seconds query_interval{300};
    static constexpr std::chrono::hours maximum_query_range{1};
    static constexpr std::chrono::days retention{7};
};

}
