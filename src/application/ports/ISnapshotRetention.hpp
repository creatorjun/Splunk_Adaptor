// src/application/ports/ISnapshotRetention.hpp
#pragma once

#include "domain/ResourceSample.hpp"

#include <chrono>
#include <cstddef>
#include <expected>

namespace monitor {

class ISnapshotRetention {
public:
    virtual ~ISnapshotRetention() = default;
    virtual std::expected<std::size_t, Error> prune(std::chrono::system_clock::time_point cutoff) = 0;
};

}
