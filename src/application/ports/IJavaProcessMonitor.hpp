// src/application/ports/IJavaProcessMonitor.hpp
#pragma once

#include "domain/JavaProcessSnapshot.hpp"

namespace monitor {

class IJavaProcessMonitor {
public:
    virtual ~IJavaProcessMonitor() = default;
    virtual JavaProcessSnapshot collect() = 0;
};

}
