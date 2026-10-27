// src/application/SnapshotRetentionSettings.hpp
#pragma once

#include <chrono>

namespace monitor {

struct SnapshotRetentionSettings {
    static constexpr std::chrono::seconds minimum_age{60};
    static constexpr std::chrono::seconds maximum_age{31536000};
    bool enabled{true};
    std::chrono::seconds max_age{3600};

    bool operator==(const SnapshotRetentionSettings&) const = default;
};

}
