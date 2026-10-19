// src/application/ports/IJavaProcessReader.hpp
#pragma once

#include "domain/JavaProcessSnapshot.hpp"

namespace monitor {

class IJavaProcessReader {
public:
    virtual ~IJavaProcessReader() = default;
    virtual JavaProcessReading read() = 0;
};

}
