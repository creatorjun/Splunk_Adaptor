// src/domain/JavaProcessSnapshot.hpp
#pragma once

#include "domain/ResourceSample.hpp"
#include "domain/ThresholdPolicy.hpp"
#include "domain/ServerRole.hpp"

#include <map>

namespace monitor {

struct JavaProcessReading {
    std::vector<std::string> running_apps;
    std::vector<Error> errors;
};

struct JavaProcessSnapshot {
    ServerRole role{ServerRole::collector};
    MetricStatus status{MetricStatus::unknown};
    std::map<std::string, std::optional<bool>> apps;
    std::vector<Error> errors;
};

}
