// tests/JsonThresholdConfigReaderTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/config/JsonThresholdConfigReader.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/resource-monitor-config-test-XXXXXX";
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

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    output.close();
    CHECK(output.good());
}

}

int main() {
    return test::run([] {
        TemporaryDirectory directory;
        const auto path = directory.path / "thresholds.json";
        monitor::JsonThresholdConfigReader reader(path);
        CHECK(!reader.read());
        write_file(path, R"({"schema_version":1,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85.5,"storage_usage_percent":90}})");
        auto policy = reader.read();
        CHECK(policy);
        CHECK(test::near(policy->cpu_usage_percent, 80));
        CHECK(test::near(policy->memory_usage_percent, 85.5));
        CHECK(test::near(policy->storage_usage_percent, 90));

        const std::vector<std::string> invalid{
            "",
            "{",
            "[]",
            R"({"schema_version":1.0,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85,"storage_usage_percent":90}})",
            R"({"schema_version":2,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85,"storage_usage_percent":90}})",
            R"({"schema_version":true,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85,"storage_usage_percent":90}})",
            R"({"schema_version":1,"thresholds":{"cpu_usage_percent":true,"memory_usage_percent":85,"storage_usage_percent":90}})",
            R"({"schema_version":1,"thresholds":{"cpu_usage_percent":"80","memory_usage_percent":85,"storage_usage_percent":90}})",
            R"({"schema_version":1,"thresholds":{"cpu_usage_percent":null,"memory_usage_percent":85,"storage_usage_percent":90}})",
            R"({"schema_version":1,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85}})",
            R"({"schema_version":1,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85,"storage_usage_percent":90,"typo":70}})",
            R"({"schema_version":1,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85,"storage_usage_percent":90},"typo":70})",
            R"({"schema_version":1,"thresholds":{"cpu_usage_percent":80,"cpu_usage_percent":20,"memory_usage_percent":85,"storage_usage_percent":90}})",
            R"({"schema_version":1,"schema_version":1,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85,"storage_usage_percent":90}})",
            R"({"schema_version":1,"thresholds":{"cpu_usage_percent":-0.1,"memory_usage_percent":85,"storage_usage_percent":90}})",
            R"({"schema_version":1,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":100.1,"storage_usage_percent":90}})",
            R"({"schema_version":1,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85,"storage_usage_percent":-1}})",
            R"({"schema_version":1,"thresholds":{"cpu_usage_percent":1e400,"memory_usage_percent":85,"storage_usage_percent":90}})",
            std::string(20, '[') + "0" + std::string(20, ']'),
            std::string(65537, ' ')
        };
        for (const auto& content : invalid) {
            write_file(path, content);
            const auto result = reader.read();
            CHECK(!result);
            CHECK(result.error().component == "threshold_config");
            CHECK(!result.error().message.empty());
        }
        write_file(path, R"({"schema_version":1,"thresholds":{"cpu_usage_percent":0,"memory_usage_percent":100,"storage_usage_percent":12.25}})");
        policy = reader.read();
        CHECK(policy);
        CHECK(policy->cpu_usage_percent == 0);
        CHECK(policy->memory_usage_percent == 100);
        CHECK(policy->storage_usage_percent == 12.25);

        auto full_limit = R"({"schema_version":1,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85,"storage_usage_percent":90}})" + std::string{};
        full_limit.resize(65536, ' ');
        write_file(path, full_limit);
        CHECK(reader.read());

        monitor::JsonThresholdConfigReader directory_reader(directory.path);
        CHECK(!directory_reader.read());
        const auto link = directory.path / "link.json";
        std::filesystem::create_symlink(path, link);
        monitor::JsonThresholdConfigReader link_reader(link);
        CHECK(!link_reader.read());
        const auto pipe = directory.path / "pipe.json";
        CHECK(::mkfifo(pipe.c_str(), 0600) == 0);
        monitor::JsonThresholdConfigReader pipe_reader(pipe);
        CHECK(!pipe_reader.read());
    });
}
