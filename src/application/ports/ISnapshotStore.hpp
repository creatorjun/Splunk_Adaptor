// src/application/ports/ISnapshotStore.hpp
#pragma once

#include "domain/ResourceSnapshot.hpp"

#include <expected>

namespace monitor {

class ISnapshotStore {
public:
    virtual ~ISnapshotStore() = default;
    virtual std::expected<void, Error> save(const ResourceSnapshot& snapshot) = 0;
};

}
