// src/infrastructure/linux/RelayLogReader.hpp
#pragma once

#include "application/ports/IRelayLogReader.hpp"

#include <memory>

namespace monitor {

class RelayLogReader final : public IRelayLogReader {
public:
    RelayLogReader();
    ~RelayLogReader() override;
    RelayLogReader(const RelayLogReader&) = delete;
    RelayLogReader& operator=(const RelayLogReader&) = delete;
    RelayLogSnapshot read(bool enabled, const std::string& path) override;
    void acknowledge() noexcept override;

private:
    class State;
    std::unique_ptr<State> state_;
};

}
