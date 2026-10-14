// src/infrastructure/linux/MountReader.hpp
#pragma once

#include "domain/ResourceSample.hpp"

#include <expected>
#include <filesystem>
#include <functional>
#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace monitor {

struct MountRecord {
    unsigned int device_major{};
    unsigned int device_minor{};
    std::string root;
    std::string mount_point;
    std::string filesystem_type;
    std::string source;

    std::string device_id() const;
};

struct MountReadResult {
    std::vector<FilesystemSample> filesystems;
    std::optional<Error> error;
};

std::expected<std::vector<MountRecord>, Error> parse_mountinfo(std::istream& input, const std::string& target = "/proc/self/mountinfo");

class MountReader {
public:
    using SourceDeviceProbe = std::function<std::expected<std::string, Error>(const std::string&)>;

    explicit MountReader(std::filesystem::path proc_root = "/proc", std::filesystem::path sys_root = "/sys",
                         SourceDeviceProbe source_probe = {});
    MountReadResult read() const;

private:
    std::filesystem::path proc_root_;
    std::filesystem::path sys_root_;
    SourceDeviceProbe source_probe_;
};

}
