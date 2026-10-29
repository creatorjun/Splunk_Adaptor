// tests/LinuxCollectorsTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/linux/MountReader.hpp"
#include "infrastructure/linux/ProcResourceReader.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = (std::filesystem::temp_directory_path() / "resource-monitor-collectors-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("Unable to create fixture directory");
        }
        path = pattern;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    std::filesystem::path path;
};

void write(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << content;
    output.close();
    CHECK(output.good());
}

std::string escape(const std::string& path) {
    std::string escaped;
    for (const char character : path) {
        switch (character) {
        case ' ': escaped += "\\040"; break;
        case '\t': escaped += "\\011"; break;
        case '\n': escaped += "\\012"; break;
        case '\\': escaped += "\\134"; break;
        default: escaped.push_back(character); break;
        }
    }
    return escaped;
}

std::string mount_line(unsigned int id, const std::string& device, const std::string& point,
                       const std::string& type, const std::string& source) {
    return std::to_string(id) + " 1 " + device + " / " + escape(point) + " rw,relatime shared:1 - " +
           type + " " + escape(source) + " rw\n";
}

void cpu_parsing() {
    std::istringstream input("cpu 100 2 30 400 5 6 7 8 999 888\ncpu0 1 2 3 4\n");
    auto result = monitor::parse_cpu_counters(input);
    CHECK(result.value);
    CHECK(!result.error);
    CHECK(result.value->ticks[0] == 100);
    CHECK(result.value->ticks[7] == 8);
    const std::vector<std::string> invalid{
        "", "cpu0 1 2 3 4 5 6 7 8", "cpu 1 2 3 4 5 6 7", "cpu -1 2 3 4 5 6 7 8",
        "cpu 1x 2 3 4 5 6 7 8", "cpu 18446744073709551616 2 3 4 5 6 7 8"
    };
    for (const auto& text : invalid) {
        std::istringstream malformed(text);
        const auto reading = monitor::parse_cpu_counters(malformed);
        CHECK(!reading.value);
        CHECK(reading.error);
    }
}

void memory_parsing() {
    std::istringstream input("MemTotal: 8192 kB\nMemFree: 4 kB\nMemAvailable: 2048 kB\n");
    const auto result = monitor::parse_memory_counters(input);
    CHECK(result.value);
    CHECK(result.value->total_bytes == 8388608);
    CHECK(result.value->available_bytes == 2097152);
    const std::vector<std::string> invalid{
        "MemTotal: 1 kB\n", "MemTotal: 1 MB\nMemAvailable: 0 kB\n",
        "MemTotal: -1 kB\nMemAvailable: 0 kB\n",
        "MemTotal: 18014398509481984 kB\nMemAvailable: 0 kB\n",
        "MemTotal: 1 kB\nMemTotal: 2 kB\nMemAvailable: 0 kB\n",
        "MemTotal: 1 kB extra\nMemAvailable: 0 kB\n",
        "MemTotal: 1 kB\nMemAvailable: 1x kB\n"
    };
    for (const auto& text : invalid) {
        std::istringstream malformed(text);
        const auto reading = monitor::parse_memory_counters(malformed);
        CHECK(!reading.value);
        CHECK(reading.error);
    }
    std::istringstream zero("MemTotal: 1 kB\nMemAvailable: 0 kB\n");
    CHECK(monitor::parse_memory_counters(zero).value->available_bytes == 0);
}

void mount_parsing() {
    std::istringstream input("31 20 253:0 /sub\\040dir /mnt/a\\040b\\134c rw shared:12 master:1 - xfs /dev/mapper/a\\040b rw\n");
    const auto result = monitor::parse_mountinfo(input);
    CHECK(result);
    CHECK(result->size() == 1);
    CHECK(result->front().device_id() == "253:0");
    CHECK(result->front().root == "/sub dir");
    CHECK(result->front().mount_point == "/mnt/a b\\c");
    CHECK(result->front().source == "/dev/mapper/a b");
    const std::vector<std::string> invalid{
        "", "31 20 253:0 / /mnt rw xfs /dev/mapper/a rw\n",
        "31 20 253:0 / /mnt rw - xfs /dev/mapper/a\n",
        "31 20 253:x / /mnt rw - xfs /dev/mapper/a rw\n",
        "31 20 253:0 / /mnt\\04 rw - xfs /dev/mapper/a rw\n",
        "31 20 253:0 / /mnt\\000 rw - xfs /dev/mapper/a rw\n",
        "31 20 253:0 / relative rw - xfs /dev/mapper/a rw\n",
        "31 20 253:0 / /mnt rw - xfs /dev/mapper/a rw extra\n"
    };
    for (const auto& text : invalid) {
        std::istringstream malformed(text);
        CHECK(!monitor::parse_mountinfo(malformed));
    }
}

void mounted_filesystems() {
    TemporaryDirectory fixture;
    const auto proc = fixture.path / "proc";
    const auto sys = fixture.path / "sys";
    const auto mounted = fixture.path / "mount with spaces";
    std::filesystem::create_directories(mounted);
    struct stat metadata {};
    CHECK(::stat(mounted.c_str(), &metadata) == 0);
    const std::string actual_device = std::to_string(::major(metadata.st_dev)) + ":" + std::to_string(::minor(metadata.st_dev));
    std::filesystem::create_directories(sys / "dev" / "block" / actual_device);
    std::filesystem::create_directories(sys / "dev" / "block" / "259:7");
    std::filesystem::create_directories(sys / "dev" / "block" / "253:8");
    const auto missing = (fixture.path / "missing").string();
    write(proc / "self" / "mountinfo",
          mount_line(30, actual_device, missing, "xfs", "/dev/mapper/ol-root") +
          mount_line(31, actual_device, mounted.string(), "xfs", "/dev/dm-0") +
          mount_line(32, actual_device, mounted.string(), "xfs", "/dev/disk/by-uuid/example") +
          mount_line(33, "259:7", missing + "-nvme", "ext4", "/dev/nvme0n1p1") +
          mount_line(34, "253:8", missing + "-lvm", "xfs", "/dev/mapper/vg-data") +
          mount_line(35, actual_device, mounted.string(), "nfs4", "server:/data") +
          mount_line(36, actual_device, mounted.string(), "tmpfs", "tmpfs") +
          mount_line(37, "0:1", "/", "overlay", "overlay") +
          mount_line(38, actual_device, mounted.string(), "fuseblk", "/dev/sdb1"));
    const auto result = monitor::MountReader(proc, sys).read();
    CHECK(!result.error);
    CHECK(result.filesystems.size() == 3);
    bool found_actual = false;
    bool found_nvme = false;
    bool found_lvm = false;
    for (const auto& filesystem : result.filesystems) {
        if (filesystem.device_id == actual_device) {
            found_actual = true;
            CHECK(filesystem.mount_points.size() == 2);
            CHECK(filesystem.mount_points[0] == missing);
            CHECK(filesystem.mount_points[1] == mounted.string());
            CHECK(filesystem.counters.value);
            CHECK(!filesystem.counters.error);
            struct statvfs expected {};
            CHECK(::statvfs(mounted.c_str(), &expected) == 0);
            CHECK(filesystem.counters.value->blocks == expected.f_blocks);
            CHECK(filesystem.counters.value->fragment_size == expected.f_frsize);
        } else {
            CHECK(!filesystem.counters.value);
            CHECK(filesystem.counters.error);
            found_nvme = found_nvme || filesystem.device == "/dev/nvme0n1p1";
            found_lvm = found_lvm || filesystem.device == "/dev/mapper/vg-data";
        }
    }
    CHECK(found_actual && found_nvme && found_lvm);
    const std::string changed_device = std::to_string(::major(metadata.st_dev)) + ":" +
                                       std::to_string(::minor(metadata.st_dev) + 1);
    std::filesystem::create_directories(sys / "dev" / "block" / changed_device);
    write(proc / "self" / "mountinfo", mount_line(40, changed_device, mounted.string(), "xfs", "/dev/changed"));
    const auto changed = monitor::MountReader(proc, sys).read();
    CHECK(changed.filesystems.size() == 1);
    CHECK(changed.filesystems.front().counters.error);
    CHECK(changed.filesystems.front().counters.error->code == "mount_changed");
}

void resource_reader() {
    TemporaryDirectory fixture;
    const auto proc = fixture.path / "proc";
    const auto sys = fixture.path / "sys";
    std::filesystem::create_directories(sys / "dev" / "block");
    write(proc / "stat", "cpu 1 2 3 4 5 6 7 8 99 99\n");
    write(proc / "meminfo", "MemTotal: 4096 kB\nMemAvailable: 1024 kB\n");
    write(proc / "self" / "mountinfo", mount_line(20, "0:1", "/", "tmpfs", "tmpfs"));
    monitor::ProcResourceReader reader(proc, sys);
    const auto sample = reader.read();
    CHECK(sample.cpu.value);
    CHECK(sample.memory.value);
    CHECK(sample.filesystems.empty());
    CHECK(!sample.filesystem_enumeration_error);
    CHECK(!sample.hostname.empty());
    CHECK(sample.timestamp.time_since_epoch().count() > 0);
    CHECK(sample.cpu_observed_at.time_since_epoch().count() > 0);
    std::filesystem::remove(proc / "stat");
    write(proc / "meminfo", "bad data\n");
    std::filesystem::remove(proc / "self" / "mountinfo");
    const auto failed = reader.read();
    CHECK(failed.cpu.error);
    CHECK(failed.memory.error);
    CHECK(failed.filesystem_enumeration_error);
    CHECK(failed.filesystems.empty());
}

void anonymous_device_fallback() {
    TemporaryDirectory fixture;
    const auto proc = fixture.path / "proc";
    const auto sys = fixture.path / "sys";
    const auto mounted = fixture.path / "btrfs-volume";
    std::filesystem::create_directories(mounted);
    struct stat metadata {};
    CHECK(::stat(mounted.c_str(), &metadata) == 0);
    const std::string actual_device = std::to_string(::major(metadata.st_dev)) + ":" + std::to_string(::minor(metadata.st_dev));
    const std::string other_device = std::to_string(::major(metadata.st_dev)) + ":" + std::to_string(::minor(metadata.st_dev) + 1);
    const std::string missing_device = std::to_string(::major(metadata.st_dev)) + ":" + std::to_string(::minor(metadata.st_dev) + 2);
    std::filesystem::create_directories(sys / "dev" / "block" / "511:12345");
    write(proc / "self" / "mountinfo",
          mount_line(50, actual_device, mounted.string(), "btrfs", "/dev/missing-alias") +
          mount_line(51, actual_device, mounted.string(), "btrfs", "/dev/nvme0n1p2") +
          mount_line(52, other_device, mounted.string(), "btrfs", "/dev/nvme0n1p2") +
          mount_line(53, missing_device, mounted.string(), "btrfs", "/dev/unavailable") +
          mount_line(54, actual_device, mounted.string(), "nfs4", "/dev/network-looking-source") +
          mount_line(55, actual_device, mounted.string(), "unknown-network-fs", "/dev/network-looking-source"));
    std::vector<std::string> probed;
    bool source_available = true;
    const monitor::MountReader reader(proc, sys, [&](const std::string& source) -> std::expected<std::string, monitor::Error> {
        probed.push_back(source);
        if (!source_available || source != "/dev/nvme0n1p2") {
            return std::unexpected(monitor::Error{"filesystems", source, "device_lookup_error", "Source device inaccessible"});
        }
        return "511:12345";
    });
    const auto result = reader.read();
    CHECK(!result.error);
    CHECK(result.filesystems.size() == 3);
    CHECK(probed.size() == 3);
    for (const auto& filesystem : result.filesystems) {
        if (filesystem.device_id == actual_device) {
            CHECK(filesystem.counters.value);
            CHECK(filesystem.mount_points.size() == 1);
        } else if (filesystem.device_id == missing_device) {
            CHECK(filesystem.counters.error);
            CHECK(filesystem.counters.error->code == "device_lookup_error");
        } else {
            CHECK(filesystem.device_id == other_device);
            CHECK(filesystem.counters.error);
            CHECK(filesystem.counters.error->code == "mount_changed");
        }
    }
    source_available = false;
    probed.clear();
    const auto refreshed = reader.read();
    CHECK(!refreshed.error);
    CHECK(probed.size() == 3);
    CHECK(refreshed.filesystems.size() == 3);
    for (const auto& filesystem : refreshed.filesystems) {
        CHECK(!filesystem.counters.value);
        CHECK(filesystem.counters.error);
        CHECK(filesystem.counters.error->code == "device_lookup_error");
    }
    const auto regular_source = fixture.path / "regular-source";
    write(regular_source, "not a block device");
    write(proc / "self" / "mountinfo", mount_line(60, actual_device, mounted.string(), "btrfs", regular_source.string()));
    const auto regular = monitor::MountReader(proc, sys).read();
    CHECK(regular.filesystems.size() == 1);
    CHECK(regular.filesystems.front().counters.error);
    CHECK(regular.filesystems.front().counters.error->code == "source_not_block_device");
    const auto missing_inventory = monitor::MountReader(proc, fixture.path / "absent-sys").read();
    CHECK(missing_inventory.error);
    CHECK(missing_inventory.error->code == "device_inventory_error");
}

}

int main() {
    return test::run([] {
        cpu_parsing();
        memory_parsing();
        mount_parsing();
        mounted_filesystems();
        resource_reader();
        anonymous_device_fallback();
    });
}
