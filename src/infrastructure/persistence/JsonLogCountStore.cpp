// src/infrastructure/persistence/JsonLogCountStore.cpp
#include "infrastructure/persistence/JsonLogCountStore.hpp"

#include "domain/LogCountPolicy.hpp"
#include "domain/LogCountValidation.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace monitor {
namespace {

using Json = nlohmann::json;
using Clock = std::chrono::system_clock;
constexpr std::size_t maximum_bytes = 8 * 1024 * 1024;

Error store_error(std::string code, std::string message) {
    return {"log_count_store", "log_counts", std::move(code), std::move(message)};
}

Error io_error(const char* operation) {
    return store_error("io_error", std::string{operation} + ": " +
        std::error_code(errno, std::generic_category()).message());
}

std::int64_t epoch(Clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}

Clock::time_point timestamp(const Json& value) {
    if (!value.is_number_integer()) throw std::runtime_error("Timestamp must be an integer");
    if (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error("Timestamp is out of range");
    }
    const auto seconds = value.get<std::int64_t>();
    const auto minimum = std::chrono::duration_cast<std::chrono::seconds>(Clock::duration::min()).count();
    const auto maximum = std::chrono::duration_cast<std::chrono::seconds>(Clock::duration::max()).count();
    if (seconds <= minimum || seconds >= maximum) {
        throw std::runtime_error("Timestamp is out of range or is not a five-minute boundary");
    }
    const auto result = Clock::time_point{std::chrono::seconds{seconds}};
    if (!valid_log_count_boundary(result)) {
        throw std::runtime_error("Timestamp is out of range or is not a five-minute boundary");
    }
    return result;
}

Json equipment_json(const std::vector<EquipmentLogVolume>& equipment) {
    Json items = Json::array();
    items.get_ref<Json::array_t&>().reserve(equipment.size());
    for (const auto& item : equipment) items.push_back({{"eqp_ip", item.eqp_ip}, {"count", item.count}});
    return items;
}

Json hour_json(const LogCountHour& hour) {
    return {{"start_epoch", epoch(hour.start)}, {"end_epoch", epoch(hour.end)},
        {"coverage_start_epoch", epoch(hour.coverage_start)}, {"completed_intervals", hour.completed_intervals},
        {"equipment", equipment_json(hour.equipment)}};
}

LogCountHour parse_hour(const Json& document) {
    LogCountHour hour;
    hour.start = timestamp(document.at("start_epoch"));
    hour.end = timestamp(document.at("end_epoch"));
    hour.coverage_start = timestamp(document.at("coverage_start_epoch"));
    const auto& completed = document.at("completed_intervals");
    if (!completed.is_number_unsigned() || completed.get<std::uint64_t>() > LogCountPolicy::intervals_per_hour) throw std::runtime_error("Invalid interval count");
    hour.completed_intervals = completed.get<unsigned int>();
    const auto& equipment = document.at("equipment");
    if (!equipment.is_array() || equipment.size() > LogCountPolicy::maximum_equipment) throw std::runtime_error("Invalid equipment list");
    hour.equipment.reserve(equipment.size());
    for (const auto& item : equipment) {
        if (!item.at("count").is_number_unsigned()) throw std::runtime_error("Invalid equipment count");
        hour.equipment.push_back({item.at("eqp_ip").get<std::string>(), item.at("count").get<std::uint64_t>(), {}});
    }
    return hour;
}

std::string local_format(Clock::time_point timestamp_value, bool filename) {
    ::tzset();
    const auto value = Clock::to_time_t(timestamp_value);
    std::tm local{};
    if (::localtime_r(&value, &local) == nullptr) throw std::runtime_error("Unable to format local timestamp");
    std::array<char, 64> buffer{};
    if (::strftime(buffer.data(), buffer.size(), filename ? "%Y%m%dT%H%M%S%z" : "%Y-%m-%dT%H:%M:%S%z", &local) == 0) {
        throw std::runtime_error("Unable to format local timestamp");
    }
    std::string result{buffer.data()};
    if (!filename) result.insert(result.size() - 2, ":");
    return result;
}

std::optional<Clock::time_point> filename_timestamp(const std::string& name) {
    if (name.size() != 33 || !name.starts_with("log_cnt_") || !name.ends_with(".json") ||
        name[16] != 'T' || (name[23] != '+' && name[23] != '-')) return std::nullopt;
    const auto number = [&](std::size_t offset, std::size_t length) -> std::optional<int> {
        int result{};
        const auto converted = std::from_chars(name.data() + offset, name.data() + offset + length, result);
        if (converted.ec != std::errc{} || converted.ptr != name.data() + offset + length || result < 0) return std::nullopt;
        return result;
    };
    const auto year = number(8, 4);
    const auto month = number(12, 2);
    const auto day = number(14, 2);
    const auto hour = number(17, 2);
    const auto minute = number(19, 2);
    const auto second = number(21, 2);
    const auto offset_hour = number(24, 2);
    const auto offset_minute = number(26, 2);
    if (!year || !month || !day || !hour || !minute || !second || !offset_hour || !offset_minute ||
        *hour > 23 || *minute > 59 || *second != 0 || *offset_hour > 23 || *offset_minute > 59) return std::nullopt;
    const std::chrono::year_month_day date{std::chrono::year{*year},
        std::chrono::month{static_cast<unsigned>(*month)}, std::chrono::day{static_cast<unsigned>(*day)}};
    if (!date.ok()) return std::nullopt;
    const auto offset = std::chrono::minutes{(*offset_hour * 60 + *offset_minute) * (name[23] == '-' ? -1 : 1)};
    const auto seconds = std::chrono::sys_days{date} + std::chrono::hours{*hour} +
        std::chrono::minutes{*minute} - offset;
    const auto minimum = std::chrono::duration_cast<std::chrono::seconds>(Clock::duration::min());
    const auto maximum = std::chrono::duration_cast<std::chrono::seconds>(Clock::duration::max());
    if (seconds.time_since_epoch() <= minimum || seconds.time_since_epoch() >= maximum) return std::nullopt;
    return Clock::time_point{seconds.time_since_epoch()};
}

std::expected<std::optional<Json>, Error> read_json(int directory, const std::string& name) {
    UniqueFd file{::openat(directory, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (!file) {
        if (errno == ENOENT) return std::optional<Json>{};
        return std::unexpected(io_error("open log count document"));
    }
    struct stat info{};
    if (::fstat(file.get(), &info) != 0) return std::unexpected(io_error("stat log count document"));
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size < 0 ||
        static_cast<std::uintmax_t>(info.st_size) > maximum_bytes) {
        return std::unexpected(store_error("invalid_file", "Log count documents must be bounded regular files"));
    }
    std::string content;
    content.reserve(static_cast<std::size_t>(info.st_size));
    std::array<char, 8192> buffer{};
    for (;;) {
        const auto count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(io_error("read log count document"));
        }
        if (count == 0) break;
        if (static_cast<std::size_t>(count) > maximum_bytes - content.size()) {
            return std::unexpected(store_error("invalid_file", "Log count document exceeds the maximum size"));
        }
        content.append(buffer.data(), static_cast<std::size_t>(count));
    }
    auto document = Json::parse(content, nullptr, false);
    if (document.is_discarded()) return std::unexpected(store_error("invalid_document", "Log count document is not valid JSON"));
    return document;
}

std::expected<void, Error> write_json(int directory, const std::string& name, const Json& document) {
    static std::atomic<std::uint64_t> sequence{};
    std::string temporary;
    UniqueFd file;
    for (unsigned int attempt = 0; attempt < 32; ++attempt) {
        temporary = ".log-count." + std::to_string(::getpid()) + "." +
            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
        file.reset(::openat(directory, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0640));
        if (file) break;
        if (errno != EEXIST) return std::unexpected(io_error("create temporary log count document"));
    }
    if (!file) return std::unexpected(store_error("name_collision", "Unable to create temporary log count document"));
    struct Cleanup {
        int directory;
        const std::string& name;
        ~Cleanup() { ::unlinkat(directory, name.c_str(), 0); }
    } cleanup{directory, temporary};
    const auto content = document.dump(2) + "\n";
    if (content.size() > maximum_bytes) return std::unexpected(store_error("invalid_document", "Log count document exceeds the maximum size"));
    std::size_t written{};
    while (written < content.size()) {
        const auto count = ::write(file.get(), content.data() + written, content.size() - written);
        if (count < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(io_error("write log count document"));
        }
        if (count == 0) return std::unexpected(store_error("io_error", "No progress writing log count document"));
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(file.get()) != 0) return std::unexpected(io_error("sync log count document"));
    if (::renameat(directory, temporary.c_str(), directory, name.c_str()) != 0) {
        return std::unexpected(io_error("replace log count document"));
    }
    if (::fsync(directory) != 0) return std::unexpected(io_error("sync log count directory"));
    return {};
}

}

std::expected<void, Error> JsonLogCountStore::open_directory(const std::string& directory) {
    if (directory_ == directory && directory_fd_ && lock_fd_) return {};
    UniqueFd output{::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (!output) return std::unexpected(io_error("open output directory"));
    if (::mkdirat(output.get(), "log_counts", 0750) != 0 && errno != EEXIST) {
        return std::unexpected(io_error("create log_counts directory"));
    }
    UniqueFd counts{::openat(output.get(), "log_counts", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (!counts) return std::unexpected(io_error("open log_counts directory"));
    if (directory_fd_ && lock_fd_) {
        struct stat current{};
        struct stat candidate{};
        if (::fstat(directory_fd_.get(), &current) != 0 || ::fstat(counts.get(), &candidate) != 0) {
            return std::unexpected(io_error("stat log_counts directory"));
        }
        if (current.st_dev == candidate.st_dev && current.st_ino == candidate.st_ino) {
            directory_ = directory;
            return {};
        }
    }
    UniqueFd lock{::openat(counts.get(), ".lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0640)};
    if (!lock) return std::unexpected(io_error("open log count lock"));
    struct stat info{};
    if (::fstat(lock.get(), &info) != 0) return std::unexpected(io_error("stat log count lock"));
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1) return std::unexpected(store_error("invalid_lock", "Log count lock must be a regular file"));
    if (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) return std::unexpected(io_error("lock log count directory"));
    directory_fd_ = std::move(counts);
    lock_fd_ = std::move(lock);
    directory_ = directory;
    return {};
}

std::expected<std::optional<LogCountState>, Error> JsonLogCountStore::load(const std::string& directory) {
    const auto opened = open_directory(directory);
    if (!opened) return std::unexpected(opened.error());
    const auto loaded = read_json(directory_fd_.get(), ".checkpoint.json");
    if (!loaded) return std::unexpected(loaded.error());
    if (!*loaded) return std::optional<LogCountState>{};
    try {
        const auto& document = **loaded;
        if (document.at("kind") != "resource-monitor-log-count-checkpoint" || document.at("schema_version") != 1) {
            throw std::runtime_error("Unknown checkpoint format");
        }
        LogCountState state;
        state.source_identity = document.at("source_identity").get<std::string>();
        state.next_start = timestamp(document.at("next_start_epoch"));
        if (!document.at("hour").is_null()) state.hour = parse_hour(document.at("hour"));
        if (!document.at("last_saved_hour_epoch").is_null()) state.last_saved_hour = timestamp(document.at("last_saved_hour_epoch"));
        if (!valid_log_count_state(state)) throw std::runtime_error("Checkpoint contains inconsistent totals or cursor");
        return state;
    } catch (const std::exception& error) {
        return std::unexpected(store_error("invalid_checkpoint", error.what()));
    }
}

std::expected<void, Error> JsonLogCountStore::commit(const std::string& directory, const LogCountState& state,
    const std::vector<LogCountReport>& reports) {
    if (!valid_log_count_state(state) || reports.size() > 2) return std::unexpected(store_error("invalid_checkpoint", "Invalid log count state"));
    const auto opened = open_directory(directory);
    if (!opened) return std::unexpected(opened.error());
    try {
        for (const auto& report : reports) {
            if (report.source_identity != state.source_identity || !valid_log_count_hour(report.hour, true) ||
                report.hour.end > state.next_start) return std::unexpected(store_error("invalid_report", "Invalid completed hour"));
            const auto filename = "log_cnt_" + local_format(report.hour.start, true) + ".json";
            const auto existing = read_json(directory_fd_.get(), filename);
            if (!existing) return std::unexpected(existing.error());
            if (*existing) {
                const auto& previous = **existing;
                if (previous.value("kind", "") != "resource-monitor-log-counts" ||
                    previous.value("source_identity", "") != report.source_identity ||
                    previous.at("window").at("start_epoch") != epoch(report.hour.start) ||
                    previous.at("coverage").at("start_epoch").get<std::int64_t>() < epoch(report.hour.coverage_start)) {
                    return std::unexpected(store_error("report_conflict", "An existing hourly file belongs to another source or wider coverage"));
                }
            }
            const Json document{
                {"kind", "resource-monitor-log-counts"}, {"schema_version", 1},
                {"source", "CloudESM.SUM_COLLECT_INFO"}, {"source_identity", report.source_identity},
                {"collected_at", local_format(report.collected_at, false)},
                {"window", {{"start", local_format(report.hour.start, false)}, {"end", local_format(report.hour.end, false)},
                    {"start_epoch", epoch(report.hour.start)}, {"end_epoch", epoch(report.hour.end)},
                    {"start_inclusive", true}, {"end_exclusive", true}}},
                {"coverage", {{"start", local_format(report.hour.coverage_start, false)},
                    {"start_epoch", epoch(report.hour.coverage_start)}, {"completed_intervals", report.hour.completed_intervals},
                    {"expected_intervals", LogCountPolicy::intervals_per_hour}, {"complete", report.hour.coverage_start == report.hour.start && report.hour.completed_intervals == LogCountPolicy::intervals_per_hour}}},
                {"equipment", equipment_json(report.hour.equipment)}};
            const auto saved = write_json(directory_fd_.get(), filename, document);
            if (!saved) return saved;
        }
        const Json checkpoint{
            {"kind", "resource-monitor-log-count-checkpoint"}, {"schema_version", 1},
            {"source_identity", state.source_identity}, {"next_start_epoch", epoch(state.next_start)},
            {"hour", state.hour ? hour_json(*state.hour) : Json{}},
            {"last_saved_hour_epoch", state.last_saved_hour ? Json(epoch(*state.last_saved_hour)) : Json{}}};
        return write_json(directory_fd_.get(), ".checkpoint.json", checkpoint);
    } catch (const std::exception& error) {
        return std::unexpected(store_error("invalid_document", error.what()));
    }
}

std::expected<std::size_t, Error> JsonLogCountStore::prune(const std::string& directory, Clock::time_point cutoff) {
    const auto opened = open_directory(directory);
    if (!opened) return std::unexpected(opened.error());
    const int descriptor = ::openat(directory_fd_.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) return std::unexpected(io_error("open log count listing"));
    auto* raw = ::fdopendir(descriptor);
    if (!raw) {
        ::close(descriptor);
        return std::unexpected(io_error("list log count directory"));
    }
    const auto close_directory = [](DIR* stream) { ::closedir(stream); };
    std::unique_ptr<DIR, decltype(close_directory)> stream{raw, close_directory};
    std::size_t removed{};
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(stream.get());
        if (!entry) {
            if (errno != 0) return std::unexpected(io_error("read log count listing"));
            break;
        }
        const std::string name{entry->d_name};
        const auto start = filename_timestamp(name);
        if (!start || *start + LogCountPolicy::report_interval > cutoff) continue;
        struct stat before{};
        if (::fstatat(directory_fd_.get(), name.c_str(), &before, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) continue;
            return std::unexpected(io_error("stat retained log count"));
        }
        if (!S_ISREG(before.st_mode) || before.st_nlink != 1) continue;
        const auto document = read_json(directory_fd_.get(), name);
        if (!document || !*document) continue;
        try {
            const auto& value = **document;
            if (value.at("kind") != "resource-monitor-log-counts" || value.at("schema_version") != 1 ||
                value.at("source") != "CloudESM.SUM_COLLECT_INFO" || !value.at("equipment").is_array() ||
                value.at("window").at("start_epoch") != epoch(*start) ||
                value.at("window").at("end_epoch") != epoch(*start + LogCountPolicy::report_interval)) continue;
        } catch (const std::exception&) {
            continue;
        }
        struct stat after{};
        if (::fstatat(directory_fd_.get(), name.c_str(), &after, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (!S_ISREG(after.st_mode) || before.st_ino != after.st_ino || before.st_dev != after.st_dev ||
            before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
            before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
            before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) continue;
        if (::unlinkat(directory_fd_.get(), name.c_str(), 0) != 0) {
            if (errno == ENOENT) continue;
            return std::unexpected(io_error("remove expired log count"));
        }
        ++removed;
    }
    if (removed > 0 && ::fsync(directory_fd_.get()) != 0) return std::unexpected(io_error("sync retained log counts"));
    return removed;
}

}
