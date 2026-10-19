// src/infrastructure/persistence/JsonSnapshotStore.cpp
#include "infrastructure/persistence/JsonSnapshotStore.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/stat.h>
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
    int release() noexcept { return std::exchange(value_, -1); }
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

std::expected<int, Error> create_temporary_file(int directory, const std::filesystem::path& path,
    std::string& name) {
    static std::atomic<std::uint64_t> sequence{};
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        name = ".resource-monitor." + std::to_string(::getpid()) + "." +
            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
        const auto file = ::openat(directory, name.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0640);
        if (file >= 0) return file;
        if (errno != EEXIST) return std::unexpected(io_error(path, "create temporary file", errno));
    }
    return std::unexpected(store_error(path, "name_collision", "Unable to allocate a temporary filename"));
}

std::expected<void, Error> write_document(int file, const std::string& document,
    const std::filesystem::path& path) {
    std::size_t written = 0;
    while (written < document.size()) {
        const auto count = ::write(file, document.data() + written, document.size() - written);
        if (count < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(io_error(path, "write temporary file", errno));
        }
        if (count == 0) return std::unexpected(io_error(path, "write temporary file", EIO));
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(file) < 0) return std::unexpected(io_error(path, "fsync temporary file", errno));
    return {};
}

std::expected<void, Error> validate_writable_directory(int directory, const std::filesystem::path& path) {
    std::string name;
    const auto created = create_temporary_file(directory, path, name);
    if (!created) return std::unexpected(created.error());
    FileDescriptor file(*created);
    TemporaryFile cleanup(directory, name);
    const auto written = write_document(file.get(), std::string(64, '\n'), path);
    if (!written) return written;
    if (file.close() < 0) return std::unexpected(io_error(path, "close temporary file", errno));
    if (::unlinkat(directory, name.c_str(), 0) < 0) {
        return std::unexpected(io_error(path, "unlink temporary file", errno));
    }
    cleanup.release();
    if (::fsync(directory) < 0) return std::unexpected(io_error(path, "fsync directory", errno));
    return {};
}

struct FormattedTime {
    std::string timestamp;
    std::string basename;
};

std::expected<FormattedTime, Error> format_time(std::chrono::system_clock::time_point timestamp,
    bool include_milliseconds, const std::filesystem::path& path) {
    const auto seconds = std::chrono::floor<std::chrono::seconds>(timestamp);
    const auto time = std::chrono::system_clock::to_time_t(seconds);
    std::tm local{};
    if (::localtime_r(&time, &local) == nullptr || local.tm_year < -1900 || local.tm_year > 8099) {
        return std::unexpected(store_error(path, "invalid_timestamp", "Timestamp is outside the supported local time range"));
    }
    std::array<char, 32> date{};
    std::array<char, 32> basename{};
    std::array<char, 8> offset{};
    if (std::strftime(date.data(), date.size(), "%Y-%m-%dT%H:%M:%S", &local) == 0 ||
        std::strftime(basename.data(), basename.size(), "%Y%m%dT%H%M%S", &local) == 0 ||
        std::strftime(offset.data(), offset.size(), "%z", &local) != 5) {
        return std::unexpected(store_error(path, "invalid_timestamp", "Unable to format local timestamp and UTC offset"));
    }
    FormattedTime result{date.data(), basename.data()};
    if (include_milliseconds) {
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp - seconds).count();
        result.timestamp += "." + std::to_string(1000 + milliseconds).substr(1);
    }
    result.timestamp += std::string(offset.data(), 3) + ":" + std::string(offset.data() + 3, 2);
    result.basename += offset.data();
    return result;
}

template <typename T>
nlohmann::json nullable(const std::optional<T>& value) {
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

nlohmann::json nullable_time(const std::optional<std::chrono::system_clock::time_point>& value) {
    if (!value) return nullptr;
    const auto formatted = format_time(*value, true, {});
    return formatted ? nlohmann::json(formatted->timestamp) : nlohmann::json(nullptr);
}

nlohmann::json serialize(const ResourceSnapshot& snapshot, const std::string& timestamp,
    const std::string& applied_at) {
    auto processes = nlohmann::json::object();
    for (const auto& [app, active] : snapshot.java_processes.apps) {
        processes[app] = nullable(active);
    }
    auto alerts = nlohmann::json::array();
    for (const auto& alert : snapshot.collect_count.alerts) {
        alerts.push_back({{"eqp_ip", alert.eqp_ip}, {"current_count", alert.current_count},
            {"previous_count", alert.previous_count}, {"current_present", alert.current_present},
            {"previous_present", alert.previous_present}, {"change_percent", nullable(alert.change_percent)},
            {"reason", alert.reason}});
    }
    nlohmann::json queried_at = nullptr;
    if (snapshot.collect_count.queried_at) {
        const auto formatted = format_time(*snapshot.collect_count.queried_at, true, {});
        if (formatted) queried_at = formatted->timestamp;
    }
    nlohmann::json analysis_error = nullptr;
    if (snapshot.collect_count.error) {
        const auto& error = *snapshot.collect_count.error;
        analysis_error = {{"component", error.component}, {"target", error.target},
            {"code", error.code}, {"message", error.message}};
    }
    auto silent_equipment = nlohmann::json::array();
    for (const auto& equipment : snapshot.log_silence.equipment) {
        silent_equipment.push_back({{"eqp_ip", equipment.eqp_ip},
            {"last_received_at", nullable_time(equipment.last_received_at)},
            {"timeout_seconds", equipment.timeout_seconds},
            {"silent_for_seconds", nullable(equipment.silent_for_seconds)},
            {"status", to_string(equipment.status)}, {"reason", equipment.reason}});
    }
    nlohmann::json silence_error = nullptr;
    if (snapshot.log_silence.error) {
        const auto& error = *snapshot.log_silence.error;
        silence_error = {{"component", error.component}, {"target", error.target},
            {"code", error.code}, {"message", error.message}};
    }
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
    nlohmann::json document{
        {"schema_version", 1},
        {"timestamp", timestamp},
        {"thresholds_applied_at", applied_at},
        {"hostname", snapshot.hostname},
        {"status", to_string(snapshot.status)},
        {"processes", std::move(processes)},
        {"process_status", to_string(snapshot.java_processes.status)},
        {"process_profile", snapshot.java_processes.is_analyzer ? "analyzer" : "collector"},
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
        {"collect_count", {
            {"source", "CloudESM.SUM_COLLECT_INFO_1DAY"},
            {"status", to_string(snapshot.collect_count.status)},
            {"current_day", snapshot.collect_count.window.current_day == 0 ? nlohmann::json(nullptr) : nlohmann::json(snapshot.collect_count.window.current_day)},
            {"previous_day", snapshot.collect_count.window.previous_day == 0 ? nlohmann::json(nullptr) : nlohmann::json(snapshot.collect_count.window.previous_day)},
            {"threshold_percent", snapshot.collect_count.threshold_percent},
            {"queried_at", std::move(queried_at)},
            {"equipment_count", snapshot.collect_count.equipment_count},
            {"alerts", std::move(alerts)},
            {"error", std::move(analysis_error)}
        }},
        {"log_silence", {
            {"source", "CloudESM.SUM_COLLECT_INFO"},
            {"status", to_string(snapshot.log_silence.status)},
            {"default_timeout_seconds", snapshot.log_silence.default_timeout_seconds},
            {"checked_at", nullable_time(snapshot.log_silence.checked_at)},
            {"queried_at", nullable_time(snapshot.log_silence.queried_at)},
            {"last_received_precision_seconds", 60},
            {"equipment_count", snapshot.log_silence.equipment.size()},
            {"equipment", std::move(silent_equipment)},
            {"error", std::move(silence_error)}
        }},
        {"errors", std::move(errors)}
    };
    if (!snapshot.processing_files.alerts.empty()) {
        auto processing_alerts = nlohmann::json::array();
        for (const auto& alert : snapshot.processing_files.alerts) {
            processing_alerts.push_back({{"path", alert.path}, {"extension", alert.extension},
                {"count", alert.count}, {"threshold", alert.threshold}});
        }
        document["processing_files"] = {
            {"status", "warning"},
            {"scan_interval_seconds", snapshot.processing_files.scan_interval_seconds},
            {"scan_started_at", nullable_time(snapshot.processing_files.scan_started_at)},
            {"scanned_at", nullable_time(snapshot.processing_files.scanned_at)},
            {"alerts", std::move(processing_alerts)}
        };
    }
    return document;
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

JsonSnapshotStore::~JsonSnapshotStore() {
    if (lock_fd_ >= 0) ::close(lock_fd_);
    if (directory_fd_ >= 0) ::close(directory_fd_);
}

std::expected<void, Error> JsonSnapshotStore::configure_output(const std::string& directory) {
    const std::lock_guard guard(mutex_);
    return configure_output_locked(directory);
}

std::expected<void, Error> JsonSnapshotStore::configure_output_locked(std::filesystem::path directory) {
    FileDescriptor candidate(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (candidate.get() < 0) return std::unexpected(io_error(directory, "open directory", errno));
    struct stat candidate_stat{};
    if (::fstat(candidate.get(), &candidate_stat) < 0) {
        return std::unexpected(io_error(directory, "stat directory", errno));
    }
    if (directory_fd_ >= 0) {
        struct stat current_stat{};
        if (::fstat(directory_fd_, &current_stat) < 0) {
            return std::unexpected(io_error(directory_, "stat directory", errno));
        }
        if (candidate_stat.st_dev == current_stat.st_dev && candidate_stat.st_ino == current_stat.st_ino) {
            const auto validated = validate_writable_directory(directory_fd_, directory);
            if (!validated) return validated;
            directory_.swap(directory);
            return {};
        }
    }
    FileDescriptor candidate_lock(::openat(candidate.get(), ".resource-monitor.lock",
        O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0640));
    if (candidate_lock.get() < 0) return std::unexpected(io_error(directory, "open lock file", errno));
    struct stat lock_stat{};
    if (::fstat(candidate_lock.get(), &lock_stat) < 0) {
        return std::unexpected(io_error(directory, "stat lock file", errno));
    }
    if (!S_ISREG(lock_stat.st_mode) || lock_stat.st_nlink != 1) {
        return std::unexpected(store_error(directory, "invalid_lock", "Lock must be a regular file with one link"));
    }
    if (::flock(candidate_lock.get(), LOCK_EX | LOCK_NB) < 0) {
        return std::unexpected(io_error(directory, "acquire single-instance lock", errno));
    }
    struct stat named_lock{};
    if (::fstatat(candidate.get(), ".resource-monitor.lock", &named_lock, AT_SYMLINK_NOFOLLOW) < 0) {
        return std::unexpected(io_error(directory, "stat lock pathname", errno));
    }
    if (!S_ISREG(named_lock.st_mode) || named_lock.st_nlink != 1 ||
        named_lock.st_dev != lock_stat.st_dev || named_lock.st_ino != lock_stat.st_ino) {
        return std::unexpected(store_error(directory, "invalid_lock", "Lock pathname changed during acquisition"));
    }
    const auto validated = validate_writable_directory(candidate.get(), directory);
    if (!validated) return validated;
    const FileDescriptor previous_directory(directory_fd_);
    const FileDescriptor previous_lock(lock_fd_);
    directory_.swap(directory);
    directory_fd_ = candidate.release();
    lock_fd_ = candidate_lock.release();
    return {};
}

std::expected<void, Error> JsonSnapshotStore::save(const ResourceSnapshot& snapshot) {
    const std::lock_guard guard(mutex_);
    if (directory_fd_ < 0) {
        const auto configured = configure_output_locked(directory_);
        if (!configured) return configured;
    }
    ::tzset();
    const auto timestamp = format_time(snapshot.timestamp, false, directory_);
    if (!timestamp) return std::unexpected(timestamp.error());
    const auto applied_at = format_time(snapshot.thresholds_applied_at, true, directory_);
    if (!applied_at) return std::unexpected(applied_at.error());
    std::string document;
    try {
        document = serialize(snapshot, timestamp->timestamp, applied_at->timestamp).dump(2, ' ', false,
            nlohmann::json::error_handler_t::replace) + "\n";
    } catch (const nlohmann::json::exception& error) {
        return std::unexpected(store_error(directory_, "serialization_error", error.what()));
    }
    std::string temporary_name;
    const auto created = create_temporary_file(directory_fd_, directory_, temporary_name);
    if (!created) return std::unexpected(created.error());
    FileDescriptor file(*created);
    TemporaryFile cleanup(directory_fd_, temporary_name);
    const auto written = write_document(file.get(), document, directory_);
    if (!written) return written;
    if (file.close() < 0) return std::unexpected(io_error(directory_, "close temporary file", errno));

    for (unsigned collision = 0; collision < 10000; ++collision) {
        const auto final_name = timestamp->basename + (collision == 0 ? "" : "_" + std::to_string(collision)) + ".json";
        bool linked = false;
        if (commit_file(directory_fd_, temporary_name.c_str(), final_name.c_str(), linked) < 0) {
            if (errno == EEXIST) continue;
            return std::unexpected(io_error(directory_ / final_name, "publish snapshot", errno));
        }
        if (linked && ::unlinkat(directory_fd_, temporary_name.c_str(), 0) < 0) {
            return std::unexpected(io_error(directory_, "unlink temporary file", errno));
        }
        cleanup.release();
        if (::fsync(directory_fd_) < 0) return std::unexpected(io_error(directory_, "fsync directory", errno));
        return {};
    }
    return std::unexpected(store_error(directory_, "name_collision", "Snapshot filename collision limit exceeded"));
}

}
