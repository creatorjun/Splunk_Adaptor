// src/application/ports/IRelayLogReader.hpp
#pragma once

#include "domain/RelayLog.hpp"

namespace monitor {

class IRelayLogReader {
public:
    virtual ~IRelayLogReader() = default;
    virtual RelayLogSnapshot read(bool enabled, const std::string& path) = 0;
    virtual void acknowledge() noexcept = 0;
};

}
