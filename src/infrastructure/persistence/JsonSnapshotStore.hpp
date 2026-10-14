// src/infrastructure/persistence/JsonSnapshotStore.hpp
#pragma once

#include "application/ports/ISnapshotStore.hpp"

#include <filesystem>

namespace monitor {

class JsonSnapshotStore final : public ISnapshotStore {
public:
    explicit JsonSnapshotStore(std::filesystem::path directory);
    std::expected<void, Error> save(const ResourceSnapshot& snapshot) override;

private:
    std::filesystem::path directory_;
};

}
