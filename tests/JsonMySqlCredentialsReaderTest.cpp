// tests/JsonMySqlCredentialsReaderTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/config/JsonMySqlCredentialsReader.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/resource-monitor-credentials-test-XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        path = created;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    std::filesystem::path path;
};

void write_file(const std::filesystem::path& path, const std::string& content, mode_t permissions = 0600) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    output.close();
    CHECK(output.good());
    CHECK(::chmod(path.c_str(), permissions) == 0);
}

void check_failure(monitor::JsonMySqlCredentialsReader& reader, const std::string& path, const std::string& code = {}) {
    const auto result = reader.read_credentials(path);
    CHECK(!result);
    CHECK(result.error().component == "mysql_credentials");
    CHECK(result.error().target == "mysql");
    CHECK(!result.error().message.empty());
    CHECK(result.error().message.find("redaction-marker") == std::string::npos);
    CHECK(result.error().message.find("private-user") == std::string::npos);
    if (!code.empty()) CHECK(result.error().code == code);
}

}

int main() {
    return test::run([] {
        TemporaryDirectory directory;
        const auto path = directory.path / "mysql.json";
        monitor::JsonMySqlCredentialsReader reader;
        check_failure(reader, path.string(), "io_error");
        const std::string valid = R"({"username":"private-user","password":"redaction-marker!$\\\"\n"})";
        for (const auto permissions : {0600, 0640}) {
            write_file(path, valid, static_cast<mode_t>(permissions));
            const auto result = reader.read_credentials(path.string());
            CHECK(result);
            CHECK(result->username == "private-user");
            CHECK(result->password == "redaction-marker!$\\\"\n");
        }
        for (const auto permissions : {0644, 0660, 0666, 0700, 0601, 0670, 04600}) {
            write_file(path, valid, static_cast<mode_t>(permissions));
            check_failure(reader, path.string(), "insecure_permissions");
        }
        write_file(path, valid);
        if (::geteuid() == 0) {
            CHECK(::chown(path.c_str(), 65534, static_cast<gid_t>(-1)) == 0);
            check_failure(reader, path.string(), "insecure_permissions");
            CHECK(::chown(path.c_str(), 0, static_cast<gid_t>(-1)) == 0);
        }
        const std::vector<std::string> invalid{
            "", "{", "[]", "null", "true", "1", "\"redaction-marker\"", "{}",
            R"({"username":"private-user"})", R"({"password":"redaction-marker"})",
            R"({"username":1,"password":"redaction-marker"})",
            R"({"username":"private-user","password":false})",
            R"({"username":"private-user","password":null})",
            R"({"username":"","password":"redaction-marker"})",
            R"({"username":"private-user","password":""})",
            R"({"username":"private-user\u0000","password":"redaction-marker"})",
            R"({"username":"private-user\n","password":"redaction-marker"})",
            R"({"username":"private-user\u007f","password":"redaction-marker"})",
            R"({"username":"private-user\u0080","password":"redaction-marker"})",
            R"({"username":"private-user","password":"redaction-marker\u0000"})",
            R"({"username":"private-user","password":"redaction-marker","extra":1})",
            R"({"username":"private-user","redaction-marker":"value"})",
            R"({"username":"private-user","username":"private-user","password":"redaction-marker"})",
            R"({"username":"private-user","password":"redaction-marker","password":"redaction-marker"})",
            R"({"username":"private-user","password":"redaction-marker" INVALID})",
            nlohmann::json{{"username", std::string(129, 'u')}, {"password", "redaction-marker"}}.dump(),
            nlohmann::json{{"username", "private-user"}, {"password", std::string(4097, 'p')}}.dump(),
            std::string(10, '[') + "0" + std::string(10, ']'),
            std::string(16385, ' ')
        };
        for (const auto& content : invalid) {
            write_file(path, content);
            check_failure(reader, path.string());
        }
        const auto limit_credentials = nlohmann::json{
            {"username", std::string(128, 'u')}, {"password", std::string(4096, 'p')}
        }.dump();
        write_file(path, limit_credentials);
        auto result = reader.read_credentials(path.string());
        CHECK(result);
        CHECK(result->username.size() == 128);
        CHECK(result->password.size() == 4096);
        auto full_limit = valid;
        full_limit.resize(16384, ' ');
        write_file(path, full_limit);
        CHECK(reader.read_credentials(path.string()));

        const auto replacement = directory.path / "mysql.replacement.json";
        write_file(replacement, R"({"username":"rotated-user","password":"rotated-password"})", 0640);
        std::filesystem::rename(replacement, path);
        result = reader.read_credentials(path.string());
        CHECK(result);
        CHECK(result->username == "rotated-user");
        CHECK(result->password == "rotated-password");

        const auto link = directory.path / "mysql.link.json";
        std::filesystem::create_symlink(path, link);
        check_failure(reader, link.string(), "io_error");
        const auto pipe = directory.path / "mysql.pipe.json";
        CHECK(::mkfifo(pipe.c_str(), 0600) == 0);
        check_failure(reader, pipe.string(), "invalid_file");
        check_failure(reader, directory.path.string(), "invalid_file");
        for (const auto& invalid_path : {std::string{}, std::string{"mysql.json"}, std::string{"/tmp/redaction-marker\n.json"},
            std::string{"/tmp/invalid\0.json", 18}}) {
            check_failure(reader, invalid_path, "invalid_path");
        }
    });
}
