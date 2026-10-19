// tests/ProcJavaProcessReaderTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/linux/ProcJavaProcessReader.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string_view>
#include <unistd.h>

namespace {

class ProcFixture {
public:
    ProcFixture() {
        root = std::filesystem::temp_directory_path() /
            ("resource-monitor-java-" + std::to_string(::getpid()) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(root);
    }
    ~ProcFixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    void write(const std::filesystem::path& file, const std::string& content) const {
        std::ofstream output(file, std::ios::binary);
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        CHECK(output.good());
    }
    void process(const std::string& pid, std::initializer_list<std::string_view> arguments,
                 char state = 'S', const std::string& name = "java") const {
        const auto directory = root / pid;
        std::filesystem::create_directory(directory);
        write(directory / "stat", pid + " (" + name + ") " + state + " 1 0 0 0\n");
        std::string command;
        for (const auto argument : arguments) {
            command.append(argument);
            command.push_back('\0');
        }
        write(directory / "cmdline", command);
    }
    std::filesystem::path root;
};

bool contains(const monitor::JavaProcessReading& reading, const std::string& app) {
    return std::ranges::find(reading.running_apps, app) != reading.running_apps.end();
}

}

int main() {
    return test::run([] {
        {
            ProcFixture fixture;
            fixture.process("100", {"/CloudESM/app/java/zulu11/bin/java", "-server", "-DAPP=SearchEngine", "Main"});
            fixture.process("101", {"java", "-Xms64m", "-DAPP=Reducer", "-classpath", ".:lib/*", "Main"});
            fixture.process("102", {"/usr/bin/java", "-DAPP=Normalizer", "-Dfile.encoding=UTF-8", "-DAPP=Normalizer", "Main"});
            fixture.process("103", {"java", "-DAPP=Reducer", "Main"});
            fixture.process("104", {"java", "-DAPP=OldName", "-DAPP=NewName", "Main"});
            fixture.process("105", {"java", "-DAPP=Removed", "-DAPP=", "Main"});
            fixture.process("106", {"java", "-DAPP=Stopped", "Main"}, 'T');
            fixture.process("107", {"java", "-DAPP=ClearedByBareProperty", "-DAPP", "Main"});
            std::filesystem::create_directory(fixture.root / "self");
            std::filesystem::create_directory(fixture.root / "thread-self");
            monitor::ProcJavaProcessReader reader(fixture.root);
            const auto result = reader.read();
            CHECK(result.errors.empty());
            CHECK(result.running_apps.size() == 5);
            CHECK(contains(result, "SearchEngine"));
            CHECK(contains(result, "Reducer"));
            CHECK(contains(result, "Normalizer"));
            CHECK(contains(result, "NewName"));
            CHECK(contains(result, "Stopped"));
            CHECK(!contains(result, "OldName"));
            CHECK(!contains(result, "Removed"));
            CHECK(!contains(result, "ClearedByBareProperty"));
        }
        {
            ProcFixture fixture;
            fixture.process("200", {"java", "-Dfile.encoding=UTF-8", "Main"});
            fixture.process("201", {"grep", "java", "-DAPP=Grep"}, 'S', "grep");
            fixture.process("202", {"bash", "-c", "java -DAPP=Shell Main"}, 'S', "bash");
            fixture.process("203", {"/usr/bin/notjava", "-DAPP=WrongExecutable", "Main"});
            fixture.process("204", {"java", "-DAPPLICATION=WrongProperty", "Main"});
            fixture.process("205", {"java", "-DAPP", "=SplitProperty", "Main"});
            fixture.process("206", {"java", "-classpath", "-DAPP=ClasspathText", "Main"});
            fixture.process("207", {"java", "Main", "-DAPP=MainArgument"});
            fixture.process("208", {"java", "-jar", "application.jar", "-DAPP=JarArgument"});
            fixture.process("209", {"java", "-m", "application/main", "-DAPP=ModuleArgument"});
            fixture.process("210", {"java", "--module=application/main", "-DAPP=ModuleEqualsArgument"});
            fixture.process("211", {"java", "-DAPP=Zombie", "Main"}, 'Z');
            fixture.process("212", {"java", "-DAPP=Dead", "Main"}, 'X');
            fixture.process("213", {"java", "-DAPP=OldDead", "Main"}, 'x');
            fixture.process("214", {"java", "-DAPP=ChangedName", "Main"}, 'S', "not) (java");
            std::filesystem::remove(fixture.root / "201" / "cmdline");
            std::filesystem::create_directory(fixture.root / "201" / "cmdline");
            const auto result = monitor::ProcJavaProcessReader(fixture.root).read();
            CHECK(result.errors.empty());
            CHECK(result.running_apps.empty());
        }
        {
            ProcFixture fixture;
            fixture.process("300", {"java", "--module-path", "-DAPP=ModulePath", "--add-opens", "java.base/java.lang=ALL-UNNAMED",
                                    "--class-path", ".:lib/*", "-DAPP=RealApplication", "Main", "-DAPP=Ignored"});
            fixture.process("301", {"java", "-DAPP=BeforeJar", "-jar", "application.jar"});
            fixture.process("302", {"java", "-DAPP=BeforeModule", "--module", "application/main"});
            const auto result = monitor::ProcJavaProcessReader(fixture.root).read();
            CHECK(result.errors.empty());
            CHECK(result.running_apps.size() == 3);
            CHECK(contains(result, "RealApplication"));
            CHECK(contains(result, "BeforeJar"));
            CHECK(contains(result, "BeforeModule"));
            CHECK(!contains(result, "ModulePath"));
        }
        {
            ProcFixture fixture;
            std::filesystem::create_directory(fixture.root / "400");
            fixture.process("401", {"java", "-DAPP=Disappeared", "Main"});
            std::filesystem::remove(fixture.root / "401" / "cmdline");
            fixture.process("402", {"java", "-DAPP=Present", "Main"});
            auto result = monitor::ProcJavaProcessReader(fixture.root).read();
            CHECK(result.errors.empty());
            CHECK(result.running_apps.size() == 1);
            CHECK(contains(result, "Present"));
            std::filesystem::remove_all(fixture.root / "402");
            result = monitor::ProcJavaProcessReader(fixture.root).read();
            CHECK(result.errors.empty());
            CHECK(result.running_apps.empty());
        }
        {
            ProcFixture fixture;
            fixture.process("500", {"java", "-DAPP=Present", "Main"});
            fixture.process("501", {"java", "-DAPP=Unreadable", "Main"});
            std::filesystem::remove(fixture.root / "501" / "cmdline");
            std::filesystem::create_directory(fixture.root / "501" / "cmdline");
            fixture.process("502", {"java", "-DAPP=BadStat", "Main"});
            fixture.write(fixture.root / "502" / "stat", "malformed stat");
            fixture.process("503", {"java", "-DAPP=BadArguments", "Main"});
            fixture.write(fixture.root / "503" / "cmdline", "java -DAPP=SecretMustNotLeak");
            fixture.process("504", {});
            const auto result = monitor::ProcJavaProcessReader(fixture.root).read();
            CHECK(result.errors.size() == 4);
            CHECK(result.running_apps.size() == 1);
            CHECK(contains(result, "Present"));
            CHECK(std::ranges::all_of(result.errors, [](const monitor::Error& error) {
                return error.component == "java_processes" && error.message.find("SecretMustNotLeak") == std::string::npos;
            }));
        }
        {
            ProcFixture fixture;
            fixture.process("600", {"java", "-DAPP=Oversized", "Main"});
            fixture.write(fixture.root / "600" / "cmdline", std::string(1024 * 1024 + 1, 'x'));
            const auto result = monitor::ProcJavaProcessReader(fixture.root).read();
            CHECK(result.errors.size() == 1);
            CHECK(result.errors[0].code == "size_limit");
            CHECK(result.running_apps.empty());
        }
        {
            ProcFixture fixture;
            const auto result = monitor::ProcJavaProcessReader(fixture.root / "missing").read();
            CHECK(result.errors.size() == 1);
            CHECK(result.errors[0].code == "read_error");
            CHECK(result.running_apps.empty());
        }
    });
}
