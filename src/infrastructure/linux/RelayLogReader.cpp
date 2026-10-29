// src/infrastructure/linux/RelayLogReader.cpp
#include "infrastructure/linux/RelayLogReader.hpp"

#include "domain/RelayLogPolicy.hpp"
#include "infrastructure/linux/UniqueFd.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <deque>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace monitor {
namespace {

constexpr std::size_t maximum_read_bytes = 1024 * 1024;
constexpr std::size_t maximum_events = 100;
constexpr std::size_t maximum_line_bytes = 16 * 1024;
constexpr std::size_t maximum_streams = 8;
constexpr std::size_t maximum_errors = 16;

struct Stream {
    UniqueFd file;
    dev_t device{};
    ino_t inode{};
    off_t offset{};
    off_t observed_size{};
    std::string line;
    bool truncated{false};
    bool skip_line{false};
    bool ignored_line{false};
    RelayLogSelection selection;
};

bool valid_path(const std::string& path) {
    return !path.empty() && path.front() == '/' &&
        std::none_of(path.begin(), path.end(), [](unsigned char value) { return value < 32 || value == 127; });
}

bool error_line(std::string_view line) {
    if (line.size() < 30 || line.substr(23, 7) != " ERROR ") return false;
    for (std::size_t index = 0; index < 23; ++index) {
        char delimiter{};
        if (index == 4 || index == 7) delimiter = '-';
        else if (index == 10) delimiter = ' ';
        else if (index == 13 || index == 16) delimiter = ':';
        else if (index == 19) delimiter = '.';
        if (delimiter != 0) {
            if (line[index] != delimiter) return false;
        } else if (line[index] < '0' || line[index] > '9') {
            return false;
        }
    }
    const auto number = [&line](std::size_t offset, std::size_t count) {
        unsigned value{};
        for (std::size_t index = 0; index < count; ++index) {
            value = value * 10 + static_cast<unsigned>(line[offset + index] - '0');
        }
        return value;
    };
    const std::chrono::year_month_day day{
        std::chrono::year{static_cast<int>(number(0, 4))},
        std::chrono::month{number(5, 2)}, std::chrono::day{number(8, 2)}};
    return day.ok() && int(day.year()) >= 1 && number(11, 2) < 24 && number(14, 2) < 60 && number(17, 2) < 60;
}

}

class RelayLogReader::State {
public:
    RelayLogSnapshot read(bool enabled, const std::string& requested_path) {
        if (!enabled) {
            reset();
            RelayLogSnapshot disabled;
            disabled.path = requested_path;
            return disabled;
        }
        if (!active_ || path_ != requested_path) {
            reset();
            active_ = true;
            path_ = requested_path;
        }
        if (!valid_path(path_)) {
            error("invalid_path", "Relay log path must be absolute and contain no control characters");
            return snapshot(false);
        }
        bool overflow = false;
        std::size_t remaining = maximum_read_bytes;
        discover(overflow, remaining);
        while (!streams_.empty() && remaining != 0 && events_.size() < maximum_events) {
            auto& stream = streams_.front();
            struct stat metadata{};
            if (::fstat(stream.file.get(), &metadata) != 0) {
                io_error("stat_failed", "fstat", errno);
                break;
            }
            if (metadata.st_size < 0) {
                error("invalid_file", "Relay log file has an invalid size");
                break;
            }
            if (metadata.st_size < stream.offset || metadata.st_size < stream.observed_size) {
                if (stream.observed_size > stream.offset) {
                    error("truncated_unread", "Log truncation removed bytes before they could be inspected");
                }
                discard_partial(stream, "Log truncation discarded an incomplete line");
                stream.offset = 0;
                stream.skip_line = false;
            }
            stream.observed_size = metadata.st_size;
            std::array<char, 8192> buffer{};
            const auto amount = std::min(remaining, buffer.size());
            ssize_t count{};
            do {
                count = ::pread(stream.file.get(), buffer.data(), amount, stream.offset);
            } while (count < 0 && errno == EINTR);
            if (count < 0) {
                io_error("read_failed", "pread", errno);
                break;
            }
            if (count == 0) {
                if (streams_.size() == 1) break;
                discard_partial(stream, "Log rotation discarded an incomplete line");
                streams_.pop_front();
                continue;
            }
            remaining -= static_cast<std::size_t>(count);
            for (std::size_t index = 0; index < static_cast<std::size_t>(count); ++index) {
                consume(stream, buffer[index]);
                ++stream.offset;
                if (events_.size() == maximum_events) break;
            }
        }
        bool backlog = overflow || streams_.size() > 1 || events_.size() == maximum_events;
        if (!streams_.empty()) {
            const auto& stream = streams_.front();
            struct stat metadata{};
            if (::fstat(stream.file.get(), &metadata) != 0) {
                io_error("stat_failed", "fstat", errno);
            } else {
                backlog = backlog || metadata.st_size > stream.offset;
            }
            backlog = backlog || !stream.line.empty() || stream.truncated;
        }
        return snapshot(backlog);
    }

    void acknowledge() noexcept {
        events_.clear();
        errors_.clear();
    }

private:
    void reset() {
        streams_.clear();
        events_.clear();
        errors_.clear();
        path_.clear();
        active_ = false;
        skip_existing_ = true;
    }

    void error(std::string code, std::string message) {
        if (std::any_of(errors_.begin(), errors_.end(), [&](const Error& existing) {
            return existing.code == code && existing.message == message;
        })) return;
        if (errors_.size() < maximum_errors) {
            errors_.push_back({"relay_log", path_, std::move(code), std::move(message)});
        } else {
            errors_.back() = {"relay_log", path_, "error_limit",
                "Further relay log errors are suppressed until acknowledgement"};
        }
    }

    void io_error(const char* code, const char* operation, int number) {
        error(code, std::string{operation} + ": " + std::error_code(number, std::generic_category()).message());
    }

    void discover(bool& overflow, std::size_t& remaining) {
        int opened{};
        do {
            opened = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        } while (opened < 0 && errno == EINTR);
        UniqueFd file{opened};
        if (!file) {
            const auto failure = errno;
            if (failure == ENOENT && streams_.empty()) skip_existing_ = false;
            io_error("open_failed", "open", failure);
            return;
        }
        struct stat metadata{};
        if (::fstat(file.get(), &metadata) != 0) {
            io_error("stat_failed", "fstat", errno);
            return;
        }
        if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0) {
            error("invalid_file", "Relay log must be a regular file with a valid size");
            return;
        }
        if (std::any_of(streams_.begin(), streams_.end(), [&](const Stream& stream) {
            return stream.device == metadata.st_dev && stream.inode == metadata.st_ino;
        })) return;
        if (streams_.size() >= maximum_streams) {
            overflow = true;
            error("rotation_limit", "Relay log rotation queue exceeded eight files; intermediate rotations may be missed");
            return;
        }
        Stream stream;
        stream.file = std::move(file);
        stream.device = metadata.st_dev;
        stream.inode = metadata.st_ino;
        stream.observed_size = metadata.st_size;
        if (streams_.empty() && skip_existing_) {
            stream.offset = metadata.st_size;
            if (metadata.st_size > 0) {
                char last{};
                ssize_t count{};
                do {
                    count = ::pread(stream.file.get(), &last, 1, metadata.st_size - 1);
                } while (count < 0 && errno == EINTR);
                if (count < 0) {
                    io_error("read_failed", "pread baseline", errno);
                    return;
                }
                remaining -= static_cast<std::size_t>(count);
                stream.skip_line = count == 1 && last != '\n';
            }
        }
        streams_.push_back(std::move(stream));
        skip_existing_ = false;
    }

    void discard_partial(Stream& stream, const char* reason) {
        if (!stream.skip_line && (!stream.line.empty() || stream.truncated)) error("incomplete_line", reason);
        stream.line.clear();
        stream.truncated = false;
        stream.ignored_line = false;
        stream.selection.reset();
    }

    void consume(Stream& stream, char value) {
        if (stream.skip_line) {
            if (value == '\n') stream.skip_line = false;
            return;
        }
        if (value != '\n') {
            if (stream.ignored_line) return;
            stream.selection.consume(value);
            if (stream.line.size() < maximum_line_bytes) stream.line.push_back(value);
            else stream.truncated = true;
            if (stream.line.size() == 30 && !RelayLogSelection::accepts_level(
                error_line(stream.line) ? RelayLogLevel::error : RelayLogLevel::other)) stream.ignored_line = true;
            return;
        }
        if (!stream.truncated && !stream.line.empty() && stream.line.back() == '\r') stream.line.pop_back();
        if (stream.selection.matches(error_line(stream.line) ? RelayLogLevel::error : RelayLogLevel::other)) {
            RelayLogEntry entry;
            entry.timestamp = stream.line.substr(0, 23);
            const auto separator = stream.line.find(" - ", 29);
            entry.message = stream.line.substr(separator == std::string::npos ? 30 : separator + 3);
            entry.line = std::move(stream.line);
            entry.truncated = stream.truncated;
            events_.push_back(std::move(entry));
        }
        stream.line.clear();
        stream.truncated = false;
        stream.ignored_line = false;
        stream.selection.reset();
    }

    RelayLogSnapshot snapshot(bool backlog) const {
        RelayLogSnapshot result;
        result.status = evaluate_relay_log(!events_.empty(), !errors_.empty(), backlog);
        result.path = path_;
        result.checked_at = std::chrono::system_clock::now();
        result.events = events_;
        result.backlog = backlog;
        result.errors = errors_;
        return result;
    }

    bool active_{};
    bool skip_existing_{true};
    std::string path_;
    std::deque<Stream> streams_;
    std::vector<RelayLogEntry> events_;
    std::vector<Error> errors_;
};

RelayLogReader::RelayLogReader() : state_(std::make_unique<State>()) {}
RelayLogReader::~RelayLogReader() = default;

RelayLogSnapshot RelayLogReader::read(bool enabled, const std::string& path) {
    return state_->read(enabled, path);
}

void RelayLogReader::acknowledge() noexcept {
    state_->acknowledge();
}

}
