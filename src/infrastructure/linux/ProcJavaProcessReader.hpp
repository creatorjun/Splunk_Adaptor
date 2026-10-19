// src/infrastructure/linux/ProcJavaProcessReader.hpp
#pragma once

#include "application/ports/IJavaProcessReader.hpp"

#include <filesystem>

namespace monitor {

class ProcJavaProcessReader final : public IJavaProcessReader {
public:
    explicit ProcJavaProcessReader(std::filesystem::path proc_root = "/proc");
    JavaProcessReading read() override;

private:
    std::filesystem::path proc_root_;
};

}
