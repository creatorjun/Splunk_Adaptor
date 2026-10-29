// tests/DirectoryProcessingFileScannerTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/linux/DirectoryProcessingFileScanner.hpp"

#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace monitor;
namespace fs = std::filesystem;

class Fixture {
public:
    Fixture() {
        char pattern[]{"/tmp/processing-files-test-XXXXXX"};
        const char* created = ::mkdtemp(pattern);
        if (!created) throw std::runtime_error("Unable to create processing file fixture");
        root = created;
        fs::create_directory(root / "collector");
        fs::create_directory(root / "normalize");
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    void file(const fs::path& relative) {
        fs::create_directories((root / relative).parent_path());
        std::ofstream output{root / relative};
        output << "fixture\n";
        CHECK(output.good());
    }
    std::vector<ProcessingDirectoryRoot> roots() const {
        return {{root / "collector", ProcessingDirectoryKind::collector},
            {root / "normalize", ProcessingDirectoryKind::normalize}};
    }
    fs::path root;
};

void recursive_counts() {
    Fixture fixture;
    fixture.file("collector/a.tmp");
    fixture.file("collector/b.done");
    fixture.file("collector/nested/c.tmp");
    fixture.file("collector/.hidden/.tmp");
    fixture.file("collector/nested/d.TMP");
    fixture.file("collector/nested/e.done.bak");
    fixture.file("collector/nested/notmp");
    fixture.file("normalize/done.done");
    fixture.file("outside/ignored.tmp");
    fs::create_directory_symlink(fixture.root / "outside", fixture.root / "collector/link");
    fs::create_symlink(fixture.root / "collector/a.tmp", fixture.root / "collector/linked.tmp");
    fs::create_symlink(fixture.root / "missing", fixture.root / "collector/dangling.done");
    CHECK(::mkfifo((fixture.root / "collector/pipe.tmp").c_str(), 0600) == 0);
    fs::create_directory(fixture.root / "collector/directory.done");
    DirectoryProcessingFileScanner scanner{fixture.roots()};
    const auto result = scanner.scan({});
    CHECK(result.status == ProcessingFilesStatus::ok);
    CHECK(result.scan_interval_seconds == 60);
    CHECK(result.scan_started_at);
    CHECK(result.scanned_at >= result.scan_started_at);
    CHECK(result.directories.size() == 2);
    CHECK(result.directories[0].kind == ProcessingDirectoryKind::collector);
    CHECK(result.directories[1].kind == ProcessingDirectoryKind::normalize);
    CHECK(result.directories[0].tmp_count == 3);
    CHECK(result.directories[0].done_count == 1);
    CHECK(result.directories[1].tmp_count == 0);
    CHECK(result.directories[1].done_count == 1);
    CHECK(result.directories[0].errors.empty());
    CHECK(result.directories[1].errors.empty());
    CHECK(fs::exists(fixture.root / "collector/a.tmp"));
    CHECK(fs::is_fifo(fixture.root / "collector/pipe.tmp"));
    CHECK(fs::is_symlink(fixture.root / "collector/linked.tmp"));
}

void errors_and_limits() {
    Fixture fixture;
    fixture.file("collector/a.tmp");
    fs::remove(fixture.root / "normalize");
    DirectoryProcessingFileScanner scanner{fixture.roots()};
    auto result = scanner.scan({});
    CHECK(result.status == ProcessingFilesStatus::partial);
    CHECK(result.directories[0].tmp_count == 1);
    CHECK(!result.directories[1].tmp_count);
    CHECK(!result.directories[1].done_count);
    CHECK(result.directories[1].errors.size() == 1);
    fs::create_directory_symlink(fixture.root / "collector", fixture.root / "normalize");
    result = scanner.scan({});
    CHECK(result.status == ProcessingFilesStatus::partial);
    CHECK(!result.directories[1].tmp_count);
    CHECK(result.directories[1].errors.size() == 1);
    fs::remove(fixture.root / "normalize");
    fixture.file("normalize");
    result = scanner.scan({});
    CHECK(result.status == ProcessingFilesStatus::partial);
    CHECK(!result.directories[1].done_count);
    fs::remove(fixture.root / "normalize");
    fs::create_directory(fixture.root / "normalize");
    fixture.file("collector/a/b/c/deep.tmp");
    ProcessingFileScanLimits limits;
    limits.max_depth = 2;
    DirectoryProcessingFileScanner limited{fixture.roots(), limits};
    result = limited.scan({});
    CHECK(result.status == ProcessingFilesStatus::partial);
    CHECK(!result.directories[0].tmp_count);
    CHECK(result.directories[0].errors[0].code == "depth_limit_exceeded");
    CHECK(result.directories[1].tmp_count == 0);
    limits.max_depth = 128;
    limits.max_entries = 1;
    DirectoryProcessingFileScanner entries{fixture.roots(), limits};
    result = entries.scan({});
    CHECK(result.status == ProcessingFilesStatus::partial);
    CHECK(result.directories[0].errors[0].code == "entry_limit_exceeded");
    std::stop_source stopped;
    stopped.request_stop();
    result = scanner.scan(stopped.get_token());
    CHECK(result.status == ProcessingFilesStatus::pending);
    CHECK(!result.scanned_at);
    CHECK(result.directories.empty());
}

void permission_error() {
    Fixture fixture;
    fixture.file("collector/blocked/a.tmp");
    fixture.file("normalize/visible.done");
    CHECK(::chmod(fixture.root.c_str(), 0755) == 0);
    CHECK(::chmod((fixture.root / "collector/blocked").c_str(), 0) == 0);
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        if (::geteuid() == 0 && (::setgid(65534) != 0 || ::setuid(65534) != 0)) ::_exit(2);
        DirectoryProcessingFileScanner scanner{fixture.roots()};
        const auto result = scanner.scan({});
        const bool valid = result.status == ProcessingFilesStatus::partial
            && !result.directories[0].tmp_count && !result.directories[0].done_count
            && result.directories[0].errors.size() == 1
            && result.directories[1].done_count == 1;
        ::_exit(valid ? 0 : 3);
    }
    int status{};
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(::chmod((fixture.root / "collector/blocked").c_str(), 0755) == 0);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

}

int main() {
    return test::run([] { recursive_counts(); errors_and_limits(); permission_error(); });
}
