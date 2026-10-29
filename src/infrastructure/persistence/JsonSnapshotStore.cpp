// src/infrastructure/persistence/JsonSnapshotStore.cpp
#include "infrastructure/persistence/JsonSnapshotStore.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <memory>
#include <optional>
#include <string_view>
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

Error retention_io_error(const std::filesystem::path& path, const char* operation, int error) {
    auto result = io_error(path, operation, error);
    result.component = "snapshot_retention";
    return result;
}

std::optional<unsigned> decimal(std::string_view text) {
    if (text.empty()) return std::nullopt;
    unsigned result{};
    for (const char character : text) {
        if (character < '0' || character > '9') return std::nullopt;
        result = result * 10 + static_cast<unsigned>(character - '0');
    }
    return result;
}

std::optional<std::chrono::sys_seconds> snapshot_time(std::string_view text) {
    if ((text.size() != 16 && text.size() != 20) || text[8] != 'T') return std::nullopt;
    const auto year = decimal(text.substr(0, 4));
    const auto month = decimal(text.substr(4, 2));
    const auto day = decimal(text.substr(6, 2));
    const auto hour = decimal(text.substr(9, 2));
    const auto minute = decimal(text.substr(11, 2));
    const auto second = decimal(text.substr(13, 2));
    if (!year || !month || !day || !hour || !minute || !second ||
        *year == 0 || *hour > 23 || *minute > 59 || *second > 59) return std::nullopt;
    const std::chrono::year_month_day date{std::chrono::year{static_cast<int>(*year)},
        std::chrono::month{*month}, std::chrono::day{*day}};
    if (!date.ok()) return std::nullopt;
    int offset{};
    if (text.size() == 16) {
        if (text[15] != 'Z') return std::nullopt;
    } else {
        if (text[15] != '+' && text[15] != '-') return std::nullopt;
        const auto offset_hour = decimal(text.substr(16, 2));
        const auto offset_minute = decimal(text.substr(18, 2));
        if (!offset_hour || !offset_minute || *offset_hour > 23 || *offset_minute > 59) return std::nullopt;
        offset = static_cast<int>(*offset_hour * 60 + *offset_minute);
        if (text[15] == '-') offset = -offset;
    }
    return std::chrono::sys_days{date} + std::chrono::hours{*hour} +
        std::chrono::minutes{*minute} + std::chrono::seconds{*second} - std::chrono::minutes{offset};
}

std::optional<std::chrono::sys_seconds> snapshot_filename_time(std::string_view name) {
    if (!name.ends_with(".json")) return std::nullopt;
    name.remove_suffix(5);
    const auto suffix = name.find('_');
    if (suffix != std::string_view::npos) {
        const auto collision = name.substr(suffix + 1);
        if (collision.empty() || collision.size() > 4 || collision.front() == '0' || !decimal(collision)) {
            return std::nullopt;
        }
        name = name.substr(0, suffix);
    }
    return snapshot_time(name);
}

std::optional<std::chrono::sys_seconds> snapshot_document_time(std::string_view text) {
    if ((text.size() != 20 && text.size() != 25) || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':') return std::nullopt;
    std::string compact{text.substr(0, 4)};
    compact += text.substr(5, 2);
    compact += text.substr(8, 2);
    compact += 'T';
    compact += text.substr(11, 2);
    compact += text.substr(14, 2);
    compact += text.substr(17, 2);
    if (text.size() == 20) {
        compact += text[19];
    } else {
        if (text[22] != ':') return std::nullopt;
        compact += text.substr(19, 3);
        compact += text.substr(23, 2);
    }
    return snapshot_time(compact);
}

bool monitor_document(const std::string& content, std::chrono::sys_seconds timestamp) {
    const auto document = nlohmann::json::parse(content, nullptr, false);
    if (document.is_discarded() || !document.is_object() ||
        !document.contains("schema_version") || !document["schema_version"].is_number_integer() ||
        document["schema_version"] != 1 || !document.contains("timestamp") ||
        !document["timestamp"].is_string() || !document.contains("hostname") ||
        !document["hostname"].is_string() || !document.contains("cpu") || !document["cpu"].is_object() ||
        !document.contains("memory") || !document["memory"].is_object() ||
        !document.contains("filesystems") || !document["filesystems"].is_array() ||
        !document.contains("errors") || !document["errors"].is_array()) return false;
    const auto recorded = snapshot_document_time(document["timestamp"].get_ref<const std::string&>());
    if (!recorded || *recorded != timestamp || !document.contains("status") ||
        !document["status"].is_string() || !document.contains("collection_status") ||
        !document["collection_status"].is_string()) return false;
    const auto& status = document["status"].get_ref<const std::string&>();
    const auto& collection_status = document["collection_status"].get_ref<const std::string&>();
    return (status == "ok" || status == "warning" || status == "unknown") &&
        (collection_status == "ok" || collection_status == "partial" || collection_status == "error");
}

bool same_regular_file(const struct stat& left, const struct stat& right) {
    return S_ISREG(left.st_mode) && S_ISREG(right.st_mode) && left.st_nlink == 1 && right.st_nlink == 1 &&
        left.st_dev == right.st_dev && left.st_ino == right.st_ino && left.st_size == right.st_size &&
        left.st_mtim.tv_sec == right.st_mtim.tv_sec && left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
        left.st_ctim.tv_sec == right.st_ctim.tv_sec && left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

std::expected<std::optional<std::string>, Error> read_retention_document(int file,
    const std::filesystem::path& path, std::size_t expected_bytes) {
    constexpr std::size_t maximum_bytes = 16 * 1024 * 1024;
    std::string content;
    content.reserve(expected_bytes);
    std::array<char, 8192> buffer{};
    for (;;) {
        const auto count = ::read(file, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(retention_io_error(path, "read snapshot", errno));
        }
        if (count == 0) return content;
        const auto bytes = static_cast<std::size_t>(count);
        if (bytes > maximum_bytes - content.size()) return std::optional<std::string>{};
        content.append(buffer.data(), bytes);
    }
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
    nlohmann::json volume_error = nullptr;
    if (snapshot.log_volume.error) {
        const auto& error = *snapshot.log_volume.error;
        volume_error = {{"component", error.component}, {"target", error.target},
            {"code", error.code}, {"message", error.message}};
    }
    auto relay_events = nlohmann::json::array();
    for (const auto& event : snapshot.relay_log.events) {
        relay_events.push_back({{"timestamp", event.timestamp}, {"message", event.message},
            {"line", event.line}, {"truncated", event.truncated}});
    }
    auto relay_errors = nlohmann::json::array();
    for (const auto& error : snapshot.relay_log.errors) {
        relay_errors.push_back({{"component", error.component}, {"target", error.target},
            {"code", error.code}, {"message", error.message}});
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
        {"role", to_string(snapshot.java_processes.role)},
        {"status", to_string(snapshot.status)},
        {"processes", std::move(processes)},
        {"process_status", to_string(snapshot.java_processes.status)},
        {"process_profile", to_string(snapshot.java_processes.role)},
        {"collection_status", to_string(snapshot.collection_status)},
        {"config_status", to_string(snapshot.config_status)},
        {"missed_intervals", snapshot.missed_intervals},
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
        {"log_volume", {
            {"source", "CloudESM.SUM_COLLECT_INFO"},
            {"status", to_string(snapshot.log_volume.status)},
            {"window_start", snapshot.log_volume.window ? nullable_time(snapshot.log_volume.window->start) : nlohmann::json(nullptr)},
            {"window_end_exclusive", snapshot.log_volume.window ? nullable_time(snapshot.log_volume.window->end) : nlohmann::json(nullptr)},
            {"queried_at", nullable_time(snapshot.log_volume.queried_at)},
            {"last_saved_hour", nullable_time(snapshot.log_volume.last_saved_hour)},
            {"completed_intervals", snapshot.log_volume.completed_intervals},
            {"backlog", snapshot.log_volume.backlog},
            {"error", std::move(volume_error)}
        }},
        {"relay_log", {
            {"status", to_string(snapshot.relay_log.status)},
            {"source", snapshot.relay_log.path},
            {"checked_at", nullable_time(snapshot.relay_log.checked_at)},
            {"error_count", snapshot.relay_log.events.size()},
            {"events", std::move(relay_events)},
            {"backlog", snapshot.relay_log.backlog},
            {"errors", std::move(relay_errors)}
        }},
        {"errors", std::move(errors)}
    };
    if (!snapshot.processing_files.alerts.empty()) {
        auto processing_alerts = nlohmann::json::array();
        for (const auto& alert : snapshot.processing_files.alerts) {
            processing_alerts.push_back({{"path", alert.path}, {"extension", alert.extension},
                {"count", alert.count}});
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

std::expected<std::size_t, Error> JsonSnapshotStore::prune(std::chrono::system_clock::time_point cutoff) {
    const std::lock_guard guard(mutex_);
    if (directory_fd_ < 0) {
        const auto configured = configure_output_locked(directory_);
        if (!configured) {
            auto error = configured.error();
            error.component = "snapshot_retention";
            return std::unexpected(std::move(error));
        }
    }
    FileDescriptor scan_file(::openat(directory_fd_, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (scan_file.get() < 0) {
        return std::unexpected(retention_io_error(directory_, "open scan directory", errno));
    }
    struct CloseDirectory {
        void operator()(DIR* directory) const noexcept { ::closedir(directory); }
    };
    std::unique_ptr<DIR, CloseDirectory> stream(::fdopendir(scan_file.get()));
    if (!stream) return std::unexpected(retention_io_error(directory_, "open directory stream", errno));
    static_cast<void>(scan_file.release());
    std::size_t removed{};
    const auto finish = [&](std::optional<Error> error = {}) -> std::expected<std::size_t, Error> {
        if (removed > 0 && ::fsync(directory_fd_) < 0 && !error) {
            error = retention_io_error(directory_, "fsync pruned directory", errno);
        }
        if (error) return std::unexpected(std::move(*error));
        return removed;
    };
    const auto cutoff_seconds = std::chrono::floor<std::chrono::seconds>(cutoff);
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(stream.get());
        if (entry == nullptr) {
            if (errno != 0) return finish(retention_io_error(directory_, "read directory stream", errno));
            return finish();
        }
        const std::string name{entry->d_name};
        const auto timestamp = snapshot_filename_time(name);
        if (!timestamp || *timestamp > cutoff_seconds) continue;
        const auto path = directory_ / name;
        struct stat named{};
        if (::fstatat(directory_fd_, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) < 0) {
            if (errno == ENOENT) continue;
            return finish(retention_io_error(path, "stat snapshot candidate", errno));
        }
        constexpr off_t maximum_bytes = 16 * 1024 * 1024;
        if (!S_ISREG(named.st_mode) || named.st_nlink != 1 || named.st_size <= 0 ||
            named.st_size > maximum_bytes) continue;
        FileDescriptor file(::openat(directory_fd_, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        if (file.get() < 0) {
            if (errno == ENOENT || errno == ELOOP || errno == ENOTDIR) continue;
            return finish(retention_io_error(path, "open snapshot candidate", errno));
        }
        struct stat opened{};
        if (::fstat(file.get(), &opened) < 0) return finish(retention_io_error(path, "stat opened snapshot", errno));
        if (!same_regular_file(named, opened)) continue;
        const auto content = read_retention_document(file.get(), path, static_cast<std::size_t>(opened.st_size));
        if (!content) return finish(content.error());
        if (!*content || !monitor_document(**content, *timestamp)) continue;
        struct stat current{};
        if (::fstat(file.get(), &current) < 0) return finish(retention_io_error(path, "recheck snapshot", errno));
        if (!same_regular_file(opened, current)) continue;
        if (::fstatat(directory_fd_, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) < 0) {
            if (errno == ENOENT) continue;
            return finish(retention_io_error(path, "recheck snapshot pathname", errno));
        }
        if (!same_regular_file(opened, current)) continue;
        if (::unlinkat(directory_fd_, name.c_str(), 0) < 0) {
            if (errno == ENOENT) continue;
            return finish(retention_io_error(path, "remove expired snapshot", errno));
        }
        ++removed;
    }
}

}
