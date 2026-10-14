// src/infrastructure/config/JsonThresholdConfigReader.hpp
#pragma once

#include "application/ports/IThresholdConfigReader.hpp"

#include <filesystem>

namespace monitor {

class JsonThresholdConfigReader final : public IThresholdConfigReader {
public:
    explicit JsonThresholdConfigReader(std::filesystem::path path);
    std::expected<ThresholdPolicy, Error> read() override;

private:
    std::filesystem::path path_;
};

}
