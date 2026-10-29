// src/domain/LogCountPolicy.hpp
#pragma once

#include <chrono>
#include <cstddef>

namespace monitor {

struct LogCountPolicy {
    static constexpr std::chrono::seconds query_interval{300};
    static constexpr std::chrono::hours maximum_query_range{1};
    static constexpr std::chrono::hours report_interval{1};
    static constexpr std::chrono::days retention{7};
    static constexpr unsigned int intervals_per_hour{12};
    static constexpr std::size_t maximum_equipment{10000};
};

}
