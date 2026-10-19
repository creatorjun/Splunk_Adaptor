// src/infrastructure/linux/ProcJavaProcessReader.cpp
#include "infrastructure/linux/ProcJavaProcessReader.hpp"

#include "infrastructure/linux/UniqueFd.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <dirent.h>
#include <expected>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace monitor {
namespace {

constexpr std::size_t maximum_processes = 131072;
constexpr std::size_t maximum_command_bytes = 1024 * 1024;
constexpr std::size_t maximum_stat_bytes = 65536;

struct DirectoryCloser {
    void operator()(DIR* directory) const noexcept { ::closedir(directory); }
};

struct ProcessState {
    std::string name;
    char state{};
};

Error read_error(const std::string& target, int number) {
    return {"java_processes", target, "read_error", std::error_code(number, std::generic_category()).message()};
}

Error parse_error(const std::string& target, const std::string& message) {
    return {"java_processes", target, "parse_error", message};
}

bool disappeared(int number) {
    return number == ENOENT || number == ESRCH;
}

bool dead(char state) {
    return state == 'Z' || state == 'X' || state == 'x';
}

std::expected<std::optional<std::string>, Error> read_file(
    int directory, const char* name, const std::string& target, std::size_t limit) {
    int opened;
    do {
        opened = ::openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    } while (opened < 0 && errno == EINTR);
    const UniqueFd descriptor(opened);
    if (!descriptor) {
        const int failure = errno;
        if (disappeared(failure)) return std::nullopt;
        return std::unexpected(read_error(target, failure));
    }
    std::string content;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto size = ::read(descriptor.get(), buffer.data(), buffer.size());
        if (size < 0) {
            const int failure = errno;
            if (failure == EINTR) continue;
            if (disappeared(failure)) return std::nullopt;
            return std::unexpected(read_error(target, failure));
        }
        if (size == 0) return content;
        const auto count = static_cast<std::size_t>(size);
        if (count > limit - content.size()) {
            return std::unexpected(Error{"java_processes", target, "size_limit", "Process metadata exceeds the read limit"});
        }
        content.append(buffer.data(), count);
    }
}

std::expected<ProcessState, Error> parse_state(
    const std::string& content, const std::string& pid, const std::string& target) {
    const std::string prefix = pid + " (";
    const auto closing = content.rfind(')');
    if (!content.starts_with(prefix) || closing == std::string::npos || closing < prefix.size() ||
        closing + 3 >= content.size() || content[closing + 1] != ' ' || content[closing + 3] != ' ' ||
        std::string_view{"RSDZTtXxKWPI"}.find(content[closing + 2]) == std::string_view::npos) {
        return std::unexpected(parse_error(target, "Process stat has an invalid identity or state"));
    }
    return ProcessState{content.substr(prefix.size(), closing - prefix.size()), content[closing + 2]};
}

bool takes_operand(std::string_view argument) {
    constexpr std::array options{
        std::string_view{"-cp"}, std::string_view{"-classpath"}, std::string_view{"--class-path"},
        std::string_view{"-p"}, std::string_view{"--module-path"}, std::string_view{"--upgrade-module-path"},
        std::string_view{"--add-modules"}, std::string_view{"--limit-modules"}, std::string_view{"--add-exports"},
        std::string_view{"--add-opens"}, std::string_view{"--add-reads"}, std::string_view{"--patch-module"},
        std::string_view{"--enable-native-access"}, std::string_view{"--source"}, std::string_view{"--describe-module"},
        std::string_view{"-d"}
    };
    return std::ranges::find(options, argument) != options.end();
}

std::expected<std::optional<std::string>, Error> parse_app(const std::string& content, const std::string& target) {
    if (content.empty() || content.back() != '\0') {
        return std::unexpected(parse_error(target, "Java command line must contain NUL-terminated arguments"));
    }
    const std::string_view bytes{content};
    const auto first_end = bytes.find('\0');
    const auto executable = bytes.substr(0, first_end);
    const auto slash = executable.rfind('/');
    if (executable.substr(slash == std::string_view::npos ? 0 : slash + 1) != "java") return std::nullopt;
    std::optional<std::string> app;
    std::size_t position = first_end + 1;
    bool skip_operand = false;
    while (position < bytes.size()) {
        const auto end = bytes.find('\0', position);
        const auto argument = bytes.substr(position, end - position);
        position = end + 1;
        if (skip_operand) {
            skip_operand = false;
            continue;
        }
        if (argument.empty() || argument.front() != '-' || argument == "--" || argument == "-jar" ||
            argument == "-m" || argument == "--module" || argument.starts_with("--module=")) break;
        if (argument == "-DAPP") {
            app.reset();
        } else if (argument.starts_with("-DAPP=")) {
            if (argument.size() == 6) app.reset(); else app = argument.substr(6);
        } else if (takes_operand(argument)) {
            skip_operand = true;
        }
    }
    return app;
}

bool numeric_pid(std::string_view name) {
    return !name.empty() && name.front() != '0' &&
        std::ranges::all_of(name, [](char character) { return character >= '0' && character <= '9'; });
}

void inspect_process(int directory, const std::filesystem::path& root, const std::string& pid,
                     JavaProcessReading& result) {
    const auto process_path = root / pid;
    int opened;
    do {
        opened = ::openat(directory, pid.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } while (opened < 0 && errno == EINTR);
    const UniqueFd process(opened);
    if (!process) {
        const int failure = errno;
        if (!disappeared(failure)) result.errors.push_back(read_error(process_path.string(), failure));
        return;
    }
    const auto stat_path = (process_path / "stat").string();
    auto stat = read_file(process.get(), "stat", stat_path, maximum_stat_bytes);
    if (!stat) {
        result.errors.push_back(stat.error());
        return;
    }
    if (!*stat) return;
    auto state = parse_state(**stat, pid, stat_path);
    if (!state) {
        result.errors.push_back(state.error());
        return;
    }
    if (state->name != "java" || dead(state->state)) return;
    const auto command_path = (process_path / "cmdline").string();
    auto command = read_file(process.get(), "cmdline", command_path, maximum_command_bytes);
    if (!command) {
        result.errors.push_back(command.error());
        return;
    }
    if (!*command) return;
    stat = read_file(process.get(), "stat", stat_path, maximum_stat_bytes);
    if (!stat) {
        result.errors.push_back(stat.error());
        return;
    }
    if (!*stat) return;
    state = parse_state(**stat, pid, stat_path);
    if (!state) {
        result.errors.push_back(state.error());
        return;
    }
    if (state->name != "java" || dead(state->state)) return;
    auto app = parse_app(**command, command_path);
    if (!app) {
        result.errors.push_back(app.error());
    } else if (*app) {
        result.running_apps.push_back(std::move(**app));
    }
}

}

ProcJavaProcessReader::ProcJavaProcessReader(std::filesystem::path proc_root) : proc_root_(std::move(proc_root)) {}

JavaProcessReading ProcJavaProcessReader::read() {
    JavaProcessReading result;
    std::unique_ptr<DIR, DirectoryCloser> directory(::opendir(proc_root_.c_str()));
    if (!directory) {
        result.errors.push_back(read_error(proc_root_.string(), errno));
        return result;
    }
    std::size_t processes{};
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(directory.get());
        if (!entry) {
            if (errno != 0) result.errors.push_back(read_error(proc_root_.string(), errno));
            break;
        }
        const std::string pid{entry->d_name};
        if (!numeric_pid(pid)) continue;
        if (++processes > maximum_processes) {
            result.errors.push_back({"java_processes", proc_root_.string(), "process_limit", "Process inventory exceeds the scan limit"});
            break;
        }
        inspect_process(::dirfd(directory.get()), proc_root_, pid, result);
    }
    std::ranges::sort(result.running_apps);
    const auto duplicate = std::ranges::unique(result.running_apps);
    result.running_apps.erase(duplicate.begin(), duplicate.end());
    return result;
}

}
