// src/domain/ResourceSample.hpp
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace monitor {

struct Error {
    std::string component;
    std::string target;
    std::string code;
    std::string message;
};

template <typename T>
struct Reading {
    std::optional<T> value;
    std::optional<Error> error;
};

struct CpuCounters {
    std::array<std::uint64_t, 8> ticks{};
};

struct MemoryCounters {
    std::uint64_t total_bytes{};
    std::uint64_t available_bytes{};
};

struct FilesystemCounters {
    std::uint64_t blocks{};
    std::uint64_t free_blocks{};
    std::uint64_t available_blocks{};
    std::uint64_t fragment_size{};
};

struct FilesystemSample {
    std::string device;
    std::string device_id;
    std::string filesystem_type;
    std::vector<std::string> mount_points;
    Reading<FilesystemCounters> counters;
};

struct ResourceSample {
    std::chrono::system_clock::time_point timestamp;
    std::chrono::steady_clock::time_point cpu_observed_at;
    std::string hostname;
    Reading<CpuCounters> cpu;
    Reading<MemoryCounters> memory;
    std::vector<FilesystemSample> filesystems;
    std::optional<Error> filesystem_enumeration_error;
};

}
