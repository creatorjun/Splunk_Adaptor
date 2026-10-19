// src/application/ProcessingFileSettings.hpp
#pragma once

#include <cstdint>

namespace monitor {

struct ProcessingFileThresholds {
    std::uint64_t tmp_count{100};
    std::uint64_t done_count{100};

    bool operator==(const ProcessingFileThresholds&) const = default;
};

struct ProcessingFileSettings {
    ProcessingFileThresholds collector{};
    ProcessingFileThresholds normalize{};

    bool operator==(const ProcessingFileSettings&) const = default;
};

}
