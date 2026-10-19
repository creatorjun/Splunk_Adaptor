// src/infrastructure/linux/DirectoryProcessingFileScanner.cpp
#include "infrastructure/linux/DirectoryProcessingFileScanner.hpp"

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace monitor {
namespace {

struct DirectoryCloser {
    void operator()(DIR* value) const noexcept { ::closedir(value); }
};

using Directory = std::unique_ptr<DIR, DirectoryCloser>;
using Clock = std::chrono::steady_clock;

struct ScanState {
    const ProcessingFileScanLimits& limits;
    std::stop_token stop;
    Clock::time_point deadline;
    std::uint64_t entries{};
    std::uint64_t tmp{};
    std::uint64_t done{};
    std::string failure;
};

bool proceed(ScanState& state) {
    if (state.stop.stop_requested()) return false;
    if (Clock::now() >= state.deadline) {
        state.failure = "scan_timeout";
        return false;
    }
    return true;
}

bool transient_entry(int code) {
    return code == ENOENT || code == ENOTDIR || code == ELOOP;
}

bool visit(Directory directory, std::size_t depth, ScanState& state) {
    const int descriptor = ::dirfd(directory.get());
    while (proceed(state)) {
        errno = 0;
        const auto* entry = ::readdir(directory.get());
        if (!entry) {
            if (errno == 0) return true;
            state.failure = "directory_read_failed";
            return false;
        }
        const std::string_view name{entry->d_name};
        if (name == "." || name == "..") continue;
        if (state.entries >= state.limits.max_entries) {
            state.failure = "entry_limit_exceeded";
            return false;
        }
        ++state.entries;
        struct stat metadata{};
        if (::fstatat(descriptor, entry->d_name, &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
            if (transient_entry(errno)) continue;
            state.failure = "entry_stat_failed";
            return false;
        }
        if (S_ISREG(metadata.st_mode)) {
            if (name.ends_with(".tmp")) ++state.tmp;
            else if (name.ends_with(".done")) ++state.done;
        } else if (S_ISDIR(metadata.st_mode)) {
            if (depth >= state.limits.max_depth) {
                state.failure = "depth_limit_exceeded";
                return false;
            }
            const int child = ::openat(descriptor, entry->d_name,
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) {
                if (transient_entry(errno)) continue;
                state.failure = "directory_open_failed";
                return false;
            }
            Directory nested{::fdopendir(child)};
            if (!nested) {
                ::close(child);
                state.failure = "directory_open_failed";
                return false;
            }
            if (!visit(std::move(nested), depth + 1, state)) return false;
        }
    }
    return false;
}

ProcessingDirectoryCount scan_root(const std::filesystem::path& path,
    const ProcessingFileScanLimits& limits, std::stop_token stop, Clock::time_point deadline) {
    ProcessingDirectoryCount result;
    result.path = path.string();
    ScanState state{limits, stop, deadline, 0, 0, 0, {}};
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        state.failure = "root_unavailable";
    } else {
        Directory root{::fdopendir(descriptor)};
        if (!root) {
            ::close(descriptor);
            state.failure = "directory_open_failed";
        } else if (visit(std::move(root), 0, state)) {
            result.tmp_count = state.tmp;
            result.done_count = state.done;
            return result;
        }
    }
    if (!stop.stop_requested()) {
        result.errors.push_back({"processing_files", result.path, state.failure,
            "Processing directory could not be completely scanned"});
    }
    return result;
}

}

DirectoryProcessingFileScanner::DirectoryProcessingFileScanner()
    : DirectoryProcessingFileScanner({"/CloudESM/data/processing/collector",
        "/CloudESM/data/processing/normalize"}) {}

DirectoryProcessingFileScanner::DirectoryProcessingFileScanner(
    std::vector<std::filesystem::path> roots, ProcessingFileScanLimits limits)
    : roots_(std::move(roots)), limits_(limits) {
    if (roots_.empty() || roots_.size() > 32 || limits_.max_depth > 128
        || limits_.max_entries == 0 || limits_.max_duration.count() <= 0) {
        throw std::invalid_argument("Invalid processing directory scan limits");
    }
}

ProcessingFilesSnapshot DirectoryProcessingFileScanner::scan(std::stop_token stop) {
    ProcessingFilesSnapshot result;
    result.status = ProcessingFilesStatus::pending;
    result.scan_started_at = std::chrono::system_clock::now();
    const auto deadline = Clock::now() + limits_.max_duration;
    std::size_t complete{};
    for (const auto& path : roots_) {
        if (stop.stop_requested()) return result;
        auto directory = scan_root(path, limits_, stop, deadline);
        if (stop.stop_requested()) return result;
        if (directory.errors.empty()) ++complete;
        result.directories.push_back(std::move(directory));
    }
    result.scanned_at = std::chrono::system_clock::now();
    result.status = complete == roots_.size() ? ProcessingFilesStatus::ok
        : complete == 0 ? ProcessingFilesStatus::error : ProcessingFilesStatus::partial;
    return result;
}

}
