// tests/RelayLogReaderTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/linux/RelayLogReader.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace monitor;

class Fixture {
public:
    Fixture() {
        std::string pattern = "/tmp/resource-monitor-relay-XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* directory = ::mkdtemp(writable.data());
        if (!directory) throw std::runtime_error("mkdtemp failed");
        root = directory;
        path = root / "CollectorForAgent.log";
    }
    ~Fixture() {
        std::error_code failure;
        std::filesystem::remove_all(root, failure);
    }
    void write(const std::filesystem::path& target, const std::string& text, bool append = false) const {
        std::ofstream output(target, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        CHECK(output.good());
    }
    void append(const std::string& text) const { write(path, text, true); }
    std::filesystem::path root;
    std::filesystem::path path;
};

std::string line(const std::string& message, const std::string& level = "ERROR") {
    return "2026-10-29 10:42:07.413 " + level + " [connection] - " + message + "\n";
}

bool has_error(const RelayLogSnapshot& snapshot, std::string_view code) {
    return std::any_of(snapshot.errors.begin(), snapshot.errors.end(), [&](const Error& error) {
        return error.code == code;
    });
}

void baseline_levels_and_acknowledgement() {
    Fixture fixture;
    fixture.write(fixture.path, line("old error"));
    RelayLogReader reader;
    auto result = reader.read(false, fixture.path.string());
    CHECK(result.status == RelayLogStatus::disabled && !result.checked_at);
    result = reader.read(true, fixture.path.string());
    CHECK(result.status == RelayLogStatus::ok && result.events.empty() && result.checked_at);
    CHECK(result.path == fixture.path.string());
    fixture.append(line("error inside INFO body", "INFO") + line("warning", "WARN") +
        line("wrong level", "ERRORS") + line("lower case", "error") +
        "2026-02-30 10:42:07.413 ERROR connection invalid date\n" +
        "2026-10-29 25:42:07.413 ERROR connection invalid hour\n" +
        "2026-10-29 10:42:07,413 ERROR connection wrong separator\n" +
        "prefix 2026-10-29 10:42:07.413 ERROR connection embedded\n" +
        "    at example.Class.method(Class.java:10)\n" +
        line("first message") +
        "2026-10-29 10:42:07.413 ERROR [CONNECTION] - second message\r\n" + line("direct message"));
    result = reader.read(true, fixture.path.string());
    CHECK(result.status == RelayLogStatus::warning && result.errors.empty());
    CHECK(result.events.size() == 3 && !result.backlog);
    CHECK(result.events[0].timestamp == "2026-10-29 10:42:07.413");
    CHECK(result.events[0].message == "first message");
    CHECK(result.events[0].line == "2026-10-29 10:42:07.413 ERROR [connection] - first message");
    CHECK(!result.events[0].truncated);
    CHECK(result.events[1].message == "second message" && result.events[1].line.back() == 'e');
    CHECK(result.events[2].message == "direct message");
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 3);
    fixture.append(line("fourth message"));
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 4 && result.events.back().message == "fourth message");
    reader.acknowledge();
    result = reader.read(true, fixture.path.string());
    CHECK(result.status == RelayLogStatus::ok && result.events.empty());
}

void connection_filter_and_stream_boundaries() {
    Fixture fixture;
    fixture.write(fixture.path, "");
    RelayLogReader reader;
    CHECK(reader.read(true, fixture.path.string()).events.empty());
    const auto raw = [](const std::string& message, const std::string& level = "ERROR") {
        return "2026-10-29 10:42:07.413 " + level + " " + message + "\n";
    };
    fixture.append(raw("disk full") + raw("connect only") + raw("connec") + raw("tion") +
        raw("Connection refused", "INFO") + raw("CONNECTION refused", "WARN") +
        raw("connection refused", "error") + raw("connection refused", "ERRORS") +
        raw("Connection refused") + raw("CONNECTION reset") + raw("cOnNeCtIoN closed") +
        raw("[ConnectionPool] source - disk full") + raw("conneconnection overlap"));
    auto result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 5 && result.errors.empty() && !result.backlog);
    CHECK(result.events[0].message == "Connection refused");
    CHECK(result.events[1].message == "CONNECTION reset");
    CHECK(result.events[2].message == "cOnNeCtIoN closed");
    CHECK(result.events[3].message == "disk full");
    CHECK(result.events[4].message == "conneconnection overlap");
    reader.acknowledge();
    fixture.append("2026-10-29 10:42:07.413 ERROR " + std::string(8170, 'x') + "CoNn");
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.empty() && result.backlog);
    fixture.append("EcTiOn split keyword\n");
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 1 && !result.backlog);
    CHECK(result.events[0].message.ends_with("CoNnEcTiOn split keyword"));
    reader.acknowledge();
    fixture.append(raw(std::string(20000, 'x') + " CONNECTION beyond retained prefix"));
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 1 && result.events[0].truncated);
    CHECK(result.events[0].line.size() == 16 * 1024 && result.events[0].line.find("CONNECTION") == std::string::npos);
    reader.acknowledge();
    fixture.append(raw(std::string(20000, 'x')) + raw("no keyword after long matching line"));
    result = reader.read(true, fixture.path.string());
    CHECK(result.status == RelayLogStatus::ok && result.events.empty() && !result.backlog);
}

void partial_lines_and_utf8() {
    Fixture fixture;
    fixture.write(fixture.path, "2026-10-29 10:42:07.413 ERROR old partial");
    RelayLogReader reader;
    CHECK(reader.read(true, fixture.path.string()).events.empty());
    fixture.append(" continued\n" + line("new after old partial"));
    auto result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 1 && result.events[0].message == "new after old partial");
    reader.acknowledge();
    const auto message = std::string{"UTF-8 한글 "} + static_cast<char>(0xff) + " tail";
    const auto text = line(message);
    fixture.append(text.substr(0, text.size() - 1));
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.empty() && result.backlog && result.status == RelayLogStatus::partial);
    fixture.append("\n");
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 1 && result.events[0].message == message);
    CHECK(result.events[0].line == text.substr(0, text.size() - 1));
    CHECK(!result.events[0].truncated && !result.backlog);
    reader.acknowledge();
    fixture.append(line(std::string(20000, 'x')) + line("after long line"));
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 2);
    CHECK(result.events[0].truncated && result.events[0].line.size() == 16 * 1024);
    CHECK(!result.events[1].truncated && result.events[1].message == "after long line");
}

void ignored_line_boundaries() {
    Fixture fixture;
    fixture.write(fixture.path, "");
    RelayLogReader reader;
    CHECK(reader.read(true, fixture.path.string()).events.empty());
    fixture.append("2026-10-29 10:42:07.413 WARN " + std::string(20000, 'x') + " connection");
    auto result = reader.read(true, fixture.path.string());
    CHECK(result.status == RelayLogStatus::partial && result.backlog && result.events.empty());
    fixture.append(" still ignored\n2026-10-29 10:42:07.413 ER");
    result = reader.read(true, fixture.path.string());
    CHECK(result.status == RelayLogStatus::partial && result.events.empty());
    fixture.append("ROR Connection after ignored line\n");
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 1 && !result.backlog && result.errors.empty());
    CHECK(result.events[0].message == "Connection after ignored line");
    reader.acknowledge();
    fixture.append("2026-10-29 10:42:07.413 INFO " + std::string(20000, 'x'));
    CHECK(reader.read(true, fixture.path.string()).status == RelayLogStatus::partial);
    std::filesystem::rename(fixture.path, fixture.root / "CollectorForAgent.log.1");
    fixture.write(fixture.path, line("after incomplete ignored line"));
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 1 && !result.backlog);
    CHECK(result.events[0].message == "after incomplete ignored line");
    CHECK(has_error(result, "incomplete_line"));
}

void path_role_and_missing_file() {
    Fixture fixture;
    RelayLogReader reader;
    auto result = reader.read(true, fixture.path.string());
    CHECK(result.status == RelayLogStatus::error && has_error(result, "open_failed"));
    CHECK(reader.read(true, fixture.path.string()).errors.size() == result.errors.size());
    fixture.write(fixture.path, line("created after missing"));
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 1 && result.events[0].message == "created after missing");
    CHECK(has_error(result, "open_failed"));
    reader.acknowledge();
    CHECK(reader.read(true, fixture.path.string()).status == RelayLogStatus::ok);
    fixture.append(line("pending before path change"));
    CHECK(reader.read(true, fixture.path.string()).events.size() == 1);
    const auto second = fixture.root / "other.log";
    fixture.write(second, line("existing second file"));
    result = reader.read(true, second.string());
    CHECK(result.events.empty() && result.status == RelayLogStatus::ok && result.path == second.string());
    fixture.write(second, line("fresh second file"), true);
    CHECK(reader.read(true, second.string()).events.size() == 1);
    CHECK(reader.read(false, second.string()).status == RelayLogStatus::disabled);
    fixture.write(second, line("written while disabled"), true);
    result = reader.read(true, second.string());
    CHECK(result.events.empty() && result.status == RelayLogStatus::ok);
    fixture.write(second, line("written after reenable"), true);
    result = reader.read(true, second.string());
    CHECK(result.events.size() == 1 && result.events[0].message == "written after reenable");
}

void rotation_and_truncation() {
    Fixture fixture;
    fixture.write(fixture.path, "");
    RelayLogReader reader;
    CHECK(reader.read(true, fixture.path.string()).status == RelayLogStatus::ok);
    const auto rotated = fixture.root / "rotated.log";
    std::filesystem::rename(fixture.path, rotated);
    fixture.write(rotated, line("old descriptor tail"), true);
    fixture.write(fixture.path, line("new inode first line"));
    auto result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 2 && result.errors.empty());
    CHECK(result.events[0].message == "old descriptor tail");
    CHECK(result.events[1].message == "new inode first line");
    reader.acknowledge();
    fixture.append(line(std::string(300, 'x')));
    CHECK(reader.read(true, fixture.path.string()).events.size() == 1);
    reader.acknowledge();
    fixture.write(fixture.path, line("after copytruncate"));
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 1 && result.events[0].message == "after copytruncate");
    CHECK(result.errors.empty());
    reader.acknowledge();
    CHECK(reader.read(true, fixture.path.string()).events.empty());
    fixture.append("2026-10-29 10:42:07.413 ERROR unfinished");
    CHECK(reader.read(true, fixture.path.string()).status == RelayLogStatus::partial);
    std::filesystem::rename(fixture.path, fixture.root / "partial-rotated.log");
    fixture.write(fixture.path, line("after incomplete rotation"));
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 1 && result.events[0].message == "after incomplete rotation");
    CHECK(has_error(result, "incomplete_line"));
}

void event_and_byte_limits() {
    Fixture fixture;
    fixture.write(fixture.path, "");
    RelayLogReader reader;
    CHECK(reader.read(true, fixture.path.string()).events.empty());
    std::string many;
    for (unsigned index = 0; index < 205; ++index) many += line("event-" + std::to_string(index));
    fixture.append(many);
    auto result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 100 && result.backlog);
    CHECK(result.events.front().message == "event-0" && result.events.back().message == "event-99");
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 100 && result.events.back().message == "event-99");
    reader.acknowledge();
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 100 && result.backlog);
    CHECK(result.events.front().message == "event-100" && result.events.back().message == "event-199");
    reader.acknowledge();
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 5 && !result.backlog);
    CHECK(result.events.front().message == "event-200" && result.events.back().message == "event-204");
    reader.acknowledge();
    fixture.append(line(std::string(1024 * 1024, 'x'), "INFO") + line("after one MiB"));
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.empty() && result.backlog && result.status == RelayLogStatus::partial);
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 1 && result.events[0].message == "after one MiB");
    CHECK(!result.backlog);
}

void bounded_rotation_queue() {
    Fixture fixture;
    fixture.write(fixture.path, "");
    RelayLogReader reader;
    CHECK(reader.read(true, fixture.path.string()).events.empty());
    std::string many;
    for (unsigned index = 0; index < 101; ++index) many += line("initial-" + std::to_string(index));
    fixture.append(many);
    CHECK(reader.read(true, fixture.path.string()).events.size() == 100);
    RelayLogSnapshot result;
    for (unsigned index = 0; index < 8; ++index) {
        std::filesystem::rename(fixture.path, fixture.root / ("rotation-" + std::to_string(index)));
        fixture.write(fixture.path, line("rotation-event-" + std::to_string(index)));
        result = reader.read(true, fixture.path.string());
        CHECK(result.events.size() == 100 && result.backlog);
    }
    CHECK(has_error(result, "rotation_limit"));
    reader.acknowledge();
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 8);
    CHECK(result.events.front().message == "initial-100");
    result = reader.read(true, fixture.path.string());
    CHECK(result.events.size() == 9 && result.events.back().message == "rotation-event-7");
    reader.acknowledge();
    CHECK(reader.read(true, fixture.path.string()).status == RelayLogStatus::ok);
}

void invalid_files_and_permissions() {
    Fixture fixture;
    RelayLogReader reader;
    CHECK(has_error(reader.read(true, fixture.root.string()), "invalid_file"));
    const auto fifo = fixture.root / "pipe";
    CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    CHECK(has_error(reader.read(true, fifo.string()), "invalid_file"));
    fixture.write(fixture.path, "");
    const auto link = fixture.root / "link";
    std::filesystem::create_symlink(fixture.path, link);
    CHECK(has_error(reader.read(true, link.string()), "open_failed"));
    for (const auto& invalid : {std::string{}, std::string{"relative.log"},
        std::string{"/tmp/invalid\n.log"}, std::string{"/tmp/a\0.log", 11}}) {
        CHECK(has_error(reader.read(true, invalid), "invalid_path"));
    }
    CHECK(::chmod(fixture.root.c_str(), 0755) == 0);
    CHECK(::chmod(fixture.path.c_str(), 0000) == 0);
    if (::geteuid() == 0) {
        const auto child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            if (::setgid(65534) != 0 || ::setuid(65534) != 0) ::_exit(2);
            try {
                RelayLogReader restricted;
                const auto denied = restricted.read(true, fixture.path.string());
                ::_exit(denied.status == RelayLogStatus::error && has_error(denied, "open_failed") ? 0 : 3);
            } catch (...) {
                ::_exit(4);
            }
        }
        int status{};
        CHECK(::waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    } else {
        CHECK(has_error(reader.read(true, fixture.path.string()), "open_failed"));
    }
    CHECK(::chmod(fixture.path.c_str(), 0600) == 0);
}

}

int main() {
    return test::run([] {
        baseline_levels_and_acknowledgement();
        connection_filter_and_stream_boundaries();
        partial_lines_and_utf8();
        ignored_line_boundaries();
        path_role_and_missing_file();
        rotation_and_truncation();
        event_and_byte_limits();
        bounded_rotation_queue();
        invalid_files_and_permissions();
        CHECK(std::string{to_string(RelayLogStatus::disabled)} == "disabled");
        CHECK(std::string{to_string(RelayLogStatus::ok)} == "ok");
        CHECK(std::string{to_string(RelayLogStatus::warning)} == "warning");
        CHECK(std::string{to_string(RelayLogStatus::error)} == "error");
        CHECK(std::string{to_string(RelayLogStatus::partial)} == "partial");
    });
}
