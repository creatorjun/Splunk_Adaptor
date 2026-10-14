// src/infrastructure/linux/ProcResourceReader.hpp
#pragma once

#include "application/ports/IResourceReader.hpp"
#include "infrastructure/linux/MountReader.hpp"

#include <filesystem>
#include <istream>
#include <string>

namespace monitor {

Reading<CpuCounters> parse_cpu_counters(std::istream& input, const std::string& target = "/proc/stat");
Reading<MemoryCounters> parse_memory_counters(std::istream& input, const std::string& target = "/proc/meminfo");

class ProcResourceReader final : public IResourceReader {
public:
    explicit ProcResourceReader(std::filesystem::path proc_root = "/proc", std::filesystem::path sys_root = "/sys");
    ResourceSample read() override;

private:
    std::filesystem::path proc_root_;
    MountReader mount_reader_;
};

}
