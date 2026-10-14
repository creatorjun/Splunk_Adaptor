// src/infrastructure/linux/ProcResourceReader.cpp
#include "infrastructure/linux/ProcResourceReader.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace monitor {
namespace {

bool parse_unsigned(std::string_view text, std::uint64_t& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

Error input_error(std::string component, const std::string& target, std::string message) {
    return {std::move(component), target, "parse_error", std::move(message)};
}

Error open_error(std::string component, const std::filesystem::path& target, int error_number) {
    return {std::move(component), target.string(), "read_error", std::error_code(error_number, std::generic_category()).message()};
}

std::string read_hostname() {
    std::array<char, 256> name{};
    if (::gethostname(name.data(), name.size() - 1) != 0) {
        return "unknown";
    }
    return name.data();
}

}

Reading<CpuCounters> parse_cpu_counters(std::istream& input, const std::string& target) {
    std::string line;
    if (!std::getline(input, line)) {
        return {{}, input_error("cpu", target, "Aggregate CPU counters are missing or unreadable")};
    }
    std::istringstream fields(line);
    std::string field;
    if (!(fields >> field) || field != "cpu") {
        return {{}, input_error("cpu", target, "The first row must contain aggregate CPU counters")};
    }
    CpuCounters counters;
    for (auto& tick : counters.ticks) {
        if (!(fields >> field) || !parse_unsigned(field, tick)) {
            return {{}, input_error("cpu", target, "Eight unsigned aggregate CPU counters are required")};
        }
    }
    if (input.bad()) {
        return {{}, input_error("cpu", target, "Failed to read aggregate CPU counters")};
    }
    return {counters, {}};
}

Reading<MemoryCounters> parse_memory_counters(std::istream& input, const std::string& target) {
    MemoryCounters counters;
    bool has_total = false;
    bool has_available = false;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string key;
        if (!(fields >> key) || (key != "MemTotal:" && key != "MemAvailable:")) {
            continue;
        }
        const bool total = key == "MemTotal:";
        if ((total && has_total) || (!total && has_available)) {
            return {{}, input_error("memory", target, "Required memory counter is duplicated")};
        }
        std::string value;
        std::string unit;
        std::string extra;
        std::uint64_t kibibytes{};
        if (!(fields >> value >> unit) || unit != "kB" || (fields >> extra) ||
            !parse_unsigned(value, kibibytes) || kibibytes > std::numeric_limits<std::uint64_t>::max() / 1024) {
            return {{}, input_error("memory", target, "Required memory counter must be an unsigned, representable kB value")};
        }
        if (total) {
            counters.total_bytes = kibibytes * 1024;
            has_total = true;
        } else {
            counters.available_bytes = kibibytes * 1024;
            has_available = true;
        }
    }
    if (input.bad()) {
        return {{}, input_error("memory", target, "Failed while reading memory counters")};
    }
    if (!has_total || !has_available) {
        return {{}, input_error("memory", target, "MemTotal and MemAvailable are required")};
    }
    return {counters, {}};
}

ProcResourceReader::ProcResourceReader(std::filesystem::path proc_root, std::filesystem::path sys_root)
    : proc_root_(std::move(proc_root)), mount_reader_(proc_root_, std::move(sys_root)) {}

ResourceSample ProcResourceReader::read() {
    ResourceSample sample;
    sample.timestamp = std::chrono::system_clock::now();
    sample.hostname = read_hostname();

    const auto cpu_path = proc_root_ / "stat";
    std::ifstream cpu_input(cpu_path);
    const int cpu_error = errno;
    sample.cpu_observed_at = std::chrono::steady_clock::now();
    sample.cpu = cpu_input ? parse_cpu_counters(cpu_input, cpu_path.string())
                           : Reading<CpuCounters>{{}, open_error("cpu", cpu_path, cpu_error)};

    const auto memory_path = proc_root_ / "meminfo";
    std::ifstream memory_input(memory_path);
    const int memory_error = errno;
    sample.memory = memory_input ? parse_memory_counters(memory_input, memory_path.string())
                                 : Reading<MemoryCounters>{{}, open_error("memory", memory_path, memory_error)};

    auto mounts = mount_reader_.read();
    sample.filesystems = std::move(mounts.filesystems);
    sample.filesystem_enumeration_error = std::move(mounts.error);
    return sample;
}

}
