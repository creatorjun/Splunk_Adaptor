// src/application/ports/IMySqlConfiguration.hpp
#pragma once

#include "application/MySqlSettings.hpp"
#include "domain/ResourceSample.hpp"

#include <expected>

namespace monitor {

class IMySqlConfiguration {
public:
    virtual ~IMySqlConfiguration() = default;
    virtual std::expected<bool, Error> prepare(const MySqlSettings& settings) = 0;
    virtual bool commit() noexcept = 0;
};

}
