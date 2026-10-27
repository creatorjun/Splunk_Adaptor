// src/application/SnapshotRetention.hpp
#pragma once

#include "application/ports/ISnapshotRetention.hpp"

namespace monitor {

class SnapshotRetention {
public:
    explicit SnapshotRetention(ISnapshotRetention& storage) noexcept;
    std::expected<std::size_t, Error> run(std::chrono::system_clock::time_point now);
    static constexpr std::chrono::seconds check_interval{60};
    static constexpr std::chrono::seconds maximum_age{3600};

private:
    ISnapshotRetention& storage_;
};

}
