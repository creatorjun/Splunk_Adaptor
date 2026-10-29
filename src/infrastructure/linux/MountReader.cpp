// src/infrastructure/linux/MountReader.cpp
#include "infrastructure/linux/MountReader.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <system_error>
#include <unordered_set>
#include <unistd.h>
#include <utility>

namespace monitor {
namespace {

class MountDescriptor {
public:
    explicit MountDescriptor(int value) : value_(value) {}
    ~MountDescriptor() {
        if (value_ >= 0) {
            ::close(value_);
        }
    }
    MountDescriptor(const MountDescriptor&) = delete;
    MountDescriptor& operator=(const MountDescriptor&) = delete;
    int get() const { return value_; }

private:
    int value_;
};

template <typename Integer>
bool parse_unsigned(std::string_view text, Integer& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

std::optional<std::string> unescape_path(std::string_view encoded) {
    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        if (encoded[index] != '\\') {
            if (encoded[index] == '\0') {
                return {};
            }
            decoded.push_back(encoded[index]);
            continue;
        }
        if (index + 3 >= encoded.size()) {
            return {};
        }
        unsigned int value{};
        for (std::size_t offset = 1; offset <= 3; ++offset) {
            const char digit = encoded[index + offset];
            if (digit < '0' || digit > '7') {
                return {};
            }
            value = value * 8 + static_cast<unsigned int>(digit - '0');
        }
        if (value == 0 || value > 255) {
            return {};
        }
        decoded.push_back(static_cast<char>(value));
        index += 3;
    }
    return decoded;
}

Error parse_error(const std::string& target, std::size_t line_number, const std::string& reason) {
    return {"filesystems", target, "parse_error", "Mountinfo line " + std::to_string(line_number) + ": " + reason};
}

Error system_error(const std::string& target, const std::string& code, int error_number) {
    return {"filesystems", target, code, std::error_code(error_number, std::generic_category()).message()};
}

bool excluded_type(std::string_view type) {
    constexpr std::array excluded{
        std::string_view{"proc"}, std::string_view{"sysfs"}, std::string_view{"tmpfs"},
        std::string_view{"devtmpfs"}, std::string_view{"overlay"}, std::string_view{"aufs"},
        std::string_view{"ramfs"}, std::string_view{"nfs"},
        std::string_view{"nfs4"}, std::string_view{"cifs"}, std::string_view{"smb3"},
        std::string_view{"9p"}, std::string_view{"ceph"}, std::string_view{"glusterfs"},
        std::string_view{"swap"}, std::string_view{"autofs"}
    };
    return type.starts_with("fuse") || std::ranges::find(excluded, type) != excluded.end();
}

bool native_block_type(std::string_view type) {
    constexpr std::array types{
        std::string_view{"btrfs"}, std::string_view{"xfs"}, std::string_view{"ext2"},
        std::string_view{"ext3"}, std::string_view{"ext4"}, std::string_view{"vfat"},
        std::string_view{"msdos"}, std::string_view{"exfat"}, std::string_view{"ntfs"},
        std::string_view{"ntfs3"}, std::string_view{"iso9660"}, std::string_view{"udf"},
        std::string_view{"reiserfs"}, std::string_view{"reiser4"}, std::string_view{"f2fs"},
        std::string_view{"jfs"}, std::string_view{"nilfs2"}, std::string_view{"bcachefs"},
        std::string_view{"ocfs2"}, std::string_view{"gfs2"}, std::string_view{"erofs"},
        std::string_view{"squashfs"}, std::string_view{"ufs"}, std::string_view{"hfs"},
        std::string_view{"hfsplus"}, std::string_view{"minix"}
    };
    return std::ranges::find(types, type) != types.end();
}

std::expected<std::string, Error> probe_source_device(const std::string& source) {
    struct stat metadata {};
    if (::stat(source.c_str(), &metadata) != 0) {
        return std::unexpected(system_error(source, "device_lookup_error", errno));
    }
    if (!S_ISBLK(metadata.st_mode)) {
        return std::unexpected(Error{"filesystems", source, "source_not_block_device", "Filesystem source is not a block device"});
    }
    return std::to_string(::major(metadata.st_rdev)) + ":" + std::to_string(::minor(metadata.st_rdev));
}

Reading<FilesystemCounters> read_counters(const MountRecord& mount) {
    const MountDescriptor descriptor(::open(mount.mount_point.c_str(), O_PATH | O_CLOEXEC));
    if (descriptor.get() < 0) {
        return {{}, system_error(mount.mount_point, "open_error", errno)};
    }
    struct stat metadata {};
    if (::fstat(descriptor.get(), &metadata) != 0) {
        return {{}, system_error(mount.mount_point, "stat_error", errno)};
    }
    if (::major(metadata.st_dev) != mount.device_major || ::minor(metadata.st_dev) != mount.device_minor) {
        return {{}, Error{"filesystems", mount.mount_point, "mount_changed", "Mounted device changed during collection"}};
    }
    struct statvfs counters {};
    if (::fstatvfs(descriptor.get(), &counters) != 0) {
        return {{}, system_error(mount.mount_point, "statvfs_error", errno)};
    }
    return {FilesystemCounters{
        static_cast<std::uint64_t>(counters.f_blocks),
        static_cast<std::uint64_t>(counters.f_bfree),
        static_cast<std::uint64_t>(counters.f_bavail),
        static_cast<std::uint64_t>(counters.f_frsize)
    }, {}};
}

}

std::string MountRecord::device_id() const {
    return std::to_string(device_major) + ":" + std::to_string(device_minor);
}

std::expected<std::vector<MountRecord>, Error> parse_mountinfo(std::istream& input, const std::string& target) {
    std::vector<MountRecord> mounts;
    std::string line;
    std::size_t line_number{};
    while (std::getline(input, line)) {
        ++line_number;
        std::istringstream stream(line);
        std::vector<std::string> fields;
        std::string field;
        while (stream >> field) {
            fields.push_back(std::move(field));
        }
        const auto separator = std::ranges::find(fields, "-");
        if (fields.size() < 10 || separator == fields.end()) {
            return std::unexpected(parse_error(target, line_number, "Missing required fields or separator"));
        }
        const auto boundary = static_cast<std::size_t>(separator - fields.begin());
        if (boundary < 6 || boundary + 4 != fields.size()) {
            return std::unexpected(parse_error(target, line_number, "Invalid field layout"));
        }
        std::uint64_t mount_id{};
        std::uint64_t parent_id{};
        if (!parse_unsigned(fields[0], mount_id) || !parse_unsigned(fields[1], parent_id) || mount_id == 0) {
            return std::unexpected(parse_error(target, line_number, "Invalid mount identifiers"));
        }
        MountRecord record;
        const auto colon = fields[2].find(':');
        const std::string_view device(fields[2]);
        if (colon == std::string::npos || !parse_unsigned(device.substr(0, colon), record.device_major) ||
            !parse_unsigned(device.substr(colon + 1), record.device_minor)) {
            return std::unexpected(parse_error(target, line_number, "Invalid device identifier"));
        }
        auto root = unescape_path(fields[3]);
        auto point = unescape_path(fields[4]);
        auto source = unescape_path(fields[boundary + 2]);
        if (!root || !point || !source || root->empty() || point->empty() || root->front() != '/' || point->front() != '/') {
            return std::unexpected(parse_error(target, line_number, "Invalid path or escape sequence"));
        }
        record.root = std::move(*root);
        record.mount_point = std::move(*point);
        record.filesystem_type = fields[boundary + 1];
        record.source = std::move(*source);
        mounts.push_back(std::move(record));
    }
    if (input.bad()) {
        return std::unexpected(Error{"filesystems", target, "read_error", "Failed while reading mountinfo"});
    }
    if (mounts.empty()) {
        return std::unexpected(Error{"filesystems", target, "parse_error", "Mountinfo contains no mount entries"});
    }
    return mounts;
}

MountReader::MountReader(std::filesystem::path proc_root, std::filesystem::path sys_root, SourceDeviceProbe source_probe)
    : proc_root_(std::move(proc_root)), sys_root_(std::move(sys_root)),
      source_probe_(source_probe ? std::move(source_probe) : SourceDeviceProbe(probe_source_device)) {}

MountReadResult MountReader::read() const {
    const auto mountinfo_path = proc_root_ / "self" / "mountinfo";
    std::ifstream input(mountinfo_path);
    if (!input) {
        return {{}, system_error(mountinfo_path.string(), "read_error", errno)};
    }
    auto mounts = parse_mountinfo(input, mountinfo_path.string());
    if (!mounts) {
        return {{}, mounts.error()};
    }
    const auto block_inventory = sys_root_ / "dev" / "block";
    std::error_code inventory_error;
    if (!std::filesystem::is_directory(block_inventory, inventory_error)) {
        return {{}, Error{"filesystems", block_inventory.string(), "device_inventory_error",
                          inventory_error ? inventory_error.message() : "Block device inventory is unavailable"}};
    }
    MountReadResult result;
    struct Candidate {
        const MountRecord* mount;
        std::optional<Error> error;
    };
    std::map<std::pair<std::string, std::string>, std::vector<Candidate>> groups;
    std::map<std::string, std::expected<bool, Error>> devices;
    const auto block_device_exists = [&](const std::string& device) -> const std::expected<bool, Error>& {
        auto [position, inserted] = devices.try_emplace(device);
        if (inserted) {
            const auto path = block_inventory / device;
            std::error_code error;
            const bool present = std::filesystem::exists(path, error);
            position->second = error ? std::expected<bool, Error>{std::unexpected(
                Error{"filesystems", path.string(), "device_lookup_error", error.message()})}
                : std::expected<bool, Error>{present};
        }
        return position->second;
    };
    std::map<std::string, std::expected<std::string, Error>> source_devices;
    for (const auto& mount : *mounts) {
        if (excluded_type(mount.filesystem_type)) {
            continue;
        }
        const auto device_id = mount.device_id();
        const auto& block_device = block_device_exists(device_id);
        if (block_device && *block_device) {
            groups[{device_id, mount.filesystem_type}].push_back({&mount, {}});
            continue;
        }
        if (!block_device) {
            groups[{device_id, mount.filesystem_type}].push_back({&mount, block_device.error()});
            continue;
        }
        if (!native_block_type(mount.filesystem_type)) {
            continue;
        }
        std::optional<Error> source_error;
        if (mount.source.empty() || mount.source.front() != '/') {
            source_error = Error{"filesystems", mount.source, "device_lookup_error", "Local filesystem source is not an absolute device path"};
        } else {
            auto [position, inserted] = source_devices.try_emplace(mount.source);
            if (inserted) position->second = source_probe_(mount.source);
            const auto& physical_device = position->second;
            if (!physical_device) {
                source_error = physical_device.error();
            } else {
                const auto& physical_exists = block_device_exists(*physical_device);
                if (!physical_exists) {
                    source_error = physical_exists.error();
                } else if (!*physical_exists) {
                    source_error = Error{"filesystems", (block_inventory / *physical_device).string(),
                        "device_lookup_error", "Source block device is absent from the device inventory"};
                }
            }
        }
        groups[{device_id, mount.filesystem_type}].push_back({&mount, std::move(source_error)});
    }
    for (const auto& [key, candidates] : groups) {
        const auto& first = *candidates.front().mount;
        FilesystemSample sample;
        sample.device = first.source;
        sample.device_id = key.first;
        sample.filesystem_type = key.second;
        std::unordered_set<std::string_view> mount_points;
        mount_points.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            const auto* mount = candidate.mount;
            if (mount_points.insert(mount->mount_point).second) {
                sample.mount_points.push_back(mount->mount_point);
            }
            if (!sample.counters.value) {
                sample.counters = candidate.error ? Reading<FilesystemCounters>{{}, candidate.error} : read_counters(*mount);
            }
        }
        result.filesystems.push_back(std::move(sample));
    }
    return result;
}

}
