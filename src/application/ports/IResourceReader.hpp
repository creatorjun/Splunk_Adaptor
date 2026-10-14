// src/application/ports/IResourceReader.hpp
#pragma once

#include "domain/ResourceSample.hpp"

namespace monitor {

class IResourceReader {
public:
    virtual ~IResourceReader() = default;
    virtual ResourceSample read() = 0;
};

}
