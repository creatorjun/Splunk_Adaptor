// src/domain/JavaProcessSnapshot.hpp
#pragma once

#include "domain/ResourceSample.hpp"
#include "domain/ThresholdPolicy.hpp"

#include <map>

namespace monitor {

struct JavaProcessReading {
    std::vector<std::string> running_apps;
    std::vector<Error> errors;
};

struct JavaProcessSnapshot {
    bool is_analyzer{};
    MetricStatus status{MetricStatus::unknown};
    std::map<std::string, std::optional<bool>> apps;
    std::vector<Error> errors;
};

}
