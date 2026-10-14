// src/infrastructure/persistence/JsonSnapshotStore.cpp
#include "infrastructure/persistence/JsonSnapshotStore.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace monitor {
namespace {

class FileDescriptor {
public:
    explicit FileDescriptor(int value = -1) noexcept : value_(value) {}
    ~FileDescriptor() { if (value_ >= 0) ::close(value_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const noexcept { return value_; }
    int close() noexcept { return value_ < 0 ? 0 : ::close(std::exchange(value_, -1)); }

private:
    int value_;
};

class TemporaryFile {
public:
    TemporaryFile(int directory, const std::string& name) noexcept : directory_(directory), name_(name) {}
    ~TemporaryFile() { if (armed_) ::unlinkat(directory_, name_.c_str(), 0); }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    void release() noexcept { armed_ = false; }

private:
    int directory_;
    const std::string& name_;
    bool armed_{true};
};

Error store_error(const std::filesystem::path& path, std::string code, std::string message) {
    return {"snapshot_store", path.string(), std::move(code), std::move(message)};
}

Error io_error(const std::filesystem::path& path, const char* operation, int error) {
    return store_error(path, "io_error", std::string(operation) + ": " +
        std::error_code(error, std::generic_category()).message());
}

std::expected<std::string, Error> format_time(std::chrono::system_clock::time_point timestamp,
    const char* format, const std::filesystem::path& path) {
    const auto seconds = std::chrono::floor<std::chrono::seconds>(timestamp);
    const auto time = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
    if (::gmtime_r(&time, &utc) == nullptr || utc.tm_year < -1900 || utc.tm_year > 8099) {
        return std::unexpected(store_error(path, "invalid_timestamp", "Timestamp is outside the supported UTC range"));
    }
    std::array<char, 32> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), format, &utc) == 0) {
        return std::unexpected(store_error(path, "invalid_timestamp", "Unable to format UTC timestamp"));
    }
    return std::string(buffer.data());
}

template <typename T>
nlohmann::json nullable(const std::optional<T>& value) {
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

nlohmann::json serialize(const ResourceSnapshot& snapshot, const std::string& timestamp,
    const std::string& applied_at) {
    auto filesystems = nlohmann::json::array();
    for (const auto& filesystem : snapshot.filesystems) {
        filesystems.push_back({
            {"status", to_string(filesystem.status)},
            {"device", filesystem.device},
            {"device_id", filesystem.device_id},
            {"filesystem_type", filesystem.filesystem_type},
            {"mount_points", filesystem.mount_points},
            {"total_bytes", nullable(filesystem.total_bytes)},
            {"used_bytes", nullable(filesystem.used_bytes)},
            {"free_bytes", nullable(filesystem.free_bytes)},
            {"available_bytes", nullable(filesystem.available_bytes)},
            {"usage_percent", nullable(filesystem.usage_percent)}
        });
    }
    auto errors = nlohmann::json::array();
    for (const auto& error : snapshot.errors) {
        errors.push_back({{"component", error.component}, {"target", error.target},
            {"code", error.code}, {"message", error.message}});
    }
    return {
        {"schema_version", 1},
        {"timestamp", timestamp},
        {"thresholds_applied_at", applied_at},
        {"hostname", snapshot.hostname},
        {"status", to_string(snapshot.status)},
        {"collection_status", to_string(snapshot.collection_status)},
        {"config_status", to_string(snapshot.config_status)},
        {"missed_intervals", snapshot.missed_intervals},
        {"applied_thresholds", {
            {"cpu_usage_percent", snapshot.applied_thresholds.cpu_usage_percent},
            {"memory_usage_percent", snapshot.applied_thresholds.memory_usage_percent},
            {"storage_usage_percent", snapshot.applied_thresholds.storage_usage_percent}
        }},
        {"cpu", {
            {"status", to_string(snapshot.cpu.status)},
            {"window_seconds", nullable(snapshot.cpu.window_seconds)},
            {"usage_percent", nullable(snapshot.cpu.usage_percent)},
            {"iowait_percent", nullable(snapshot.cpu.iowait_percent)},
            {"steal_percent", nullable(snapshot.cpu.steal_percent)}
        }},
        {"memory", {
            {"status", to_string(snapshot.memory.status)},
            {"total_bytes", nullable(snapshot.memory.total_bytes)},
            {"used_bytes", nullable(snapshot.memory.used_bytes)},
            {"available_bytes", nullable(snapshot.memory.available_bytes)},
            {"usage_percent", nullable(snapshot.memory.usage_percent)}
        }},
        {"filesystems", std::move(filesystems)},
        {"errors", std::move(errors)}
    };
}

int commit_file(int directory, const char* temporary_name, const char* final_name, bool& linked) {
    linked = false;
#ifdef SYS_renameat2
    if (::syscall(SYS_renameat2, directory, temporary_name, directory, final_name, RENAME_NOREPLACE) == 0) {
        return 0;
    }
    if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) return -1;
#endif
    if (::linkat(directory, temporary_name, directory, final_name, 0) < 0) return -1;
    linked = true;
    return 0;
}

}

JsonSnapshotStore::JsonSnapshotStore(std::filesystem::path directory) : directory_(std::move(directory)) {}

std::expected<void, Error> JsonSnapshotStore::save(const ResourceSnapshot& snapshot) {
    const auto timestamp = format_time(snapshot.timestamp, "%Y-%m-%dT%H:%M:%SZ", directory_);
    if (!timestamp) return std::unexpected(timestamp.error());
    auto applied_at = format_time(snapshot.thresholds_applied_at, "%Y-%m-%dT%H:%M:%S", directory_);
    if (!applied_at) return std::unexpected(applied_at.error());
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        snapshot.thresholds_applied_at - std::chrono::floor<std::chrono::seconds>(snapshot.thresholds_applied_at)).count();
    *applied_at += "." + std::to_string(1000 + milliseconds).substr(1) + "Z";
    const auto basename = format_time(snapshot.timestamp, "%Y%m%dT%H%M%SZ", directory_);
    if (!basename) return std::unexpected(basename.error());
    std::string document;
    try {
        document = serialize(snapshot, *timestamp, *applied_at).dump(2, ' ', false,
            nlohmann::json::error_handler_t::replace) + "\n";
    } catch (const nlohmann::json::exception& error) {
        return std::unexpected(store_error(directory_, "serialization_error", error.what()));
    }
    const FileDescriptor directory(::open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.get() < 0) return std::unexpected(io_error(directory_, "open directory", errno));

    static std::atomic<std::uint64_t> temporary_sequence{};
    std::string temporary_name;
    int temporary_fd = -1;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        temporary_name = ".resource-monitor." + std::to_string(::getpid()) + "." +
            std::to_string(temporary_sequence.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
        temporary_fd = ::openat(directory.get(), temporary_name.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0640);
        if (temporary_fd >= 0) break;
        if (errno != EEXIST) return std::unexpected(io_error(directory_, "create temporary file", errno));
    }
    if (temporary_fd < 0) {
        return std::unexpected(store_error(directory_, "name_collision", "Unable to allocate a temporary filename"));
    }
    FileDescriptor file(temporary_fd);
    TemporaryFile cleanup(directory.get(), temporary_name);
    std::size_t written = 0;
    while (written < document.size()) {
        const auto count = ::write(file.get(), document.data() + written, document.size() - written);
        if (count < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(io_error(directory_, "write temporary file", errno));
        }
        if (count == 0) return std::unexpected(io_error(directory_, "write temporary file", EIO));
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(file.get()) < 0) return std::unexpected(io_error(directory_, "fsync temporary file", errno));
    if (file.close() < 0) return std::unexpected(io_error(directory_, "close temporary file", errno));

    for (unsigned collision = 0; collision < 10000; ++collision) {
        const auto final_name = *basename + (collision == 0 ? "" : "_" + std::to_string(collision)) + ".json";
        bool linked = false;
        if (commit_file(directory.get(), temporary_name.c_str(), final_name.c_str(), linked) < 0) {
            if (errno == EEXIST) continue;
            return std::unexpected(io_error(directory_ / final_name, "publish snapshot", errno));
        }
        if (linked && ::unlinkat(directory.get(), temporary_name.c_str(), 0) < 0) {
            return std::unexpected(io_error(directory_, "unlink temporary file", errno));
        }
        cleanup.release();
        if (::fsync(directory.get()) < 0) return std::unexpected(io_error(directory_, "fsync directory", errno));
        return {};
    }
    return std::unexpected(store_error(directory_, "name_collision", "Snapshot filename collision limit exceeded"));
}

}
