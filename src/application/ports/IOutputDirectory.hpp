// src/application/ports/IOutputDirectory.hpp
#pragma once

#include "domain/ResourceSample.hpp"

#include <expected>
#include <string>

namespace monitor {

class IOutputDirectory {
public:
    virtual ~IOutputDirectory() = default;
    virtual std::expected<void, Error> configure_output(const std::string& directory) = 0;
};

}
