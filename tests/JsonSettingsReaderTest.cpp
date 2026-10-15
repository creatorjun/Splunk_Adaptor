// tests/JsonSettingsReaderTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/config/JsonSettingsReader.hpp"

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

std::string document_with_collection(const std::string& collection) {
    return R"({"schema_version":1,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85.5,"storage_usage_percent":90},"collection":)"
        + collection + "}";
}

void check_failure(monitor::JsonSettingsReader& reader, const std::filesystem::path& path) {
    const auto settings = reader.read_settings();
    CHECK(!settings);
    CHECK(settings.error().component == "settings_config");
    CHECK(settings.error().target == path.string());
    CHECK(!settings.error().message.empty());
    const auto thresholds = reader.read();
    CHECK(!thresholds);
    CHECK(thresholds.error().component == "settings_config");
    CHECK(thresholds.error().target == path.string());
    CHECK(!thresholds.error().message.empty());
}

}

int main() {
    return test::run([] {
        TemporaryDirectory directory;
        const auto path = directory.path / "settings.json";
        monitor::JsonSettingsReader reader(path);
        check_failure(reader, path);
        write_file(path, R"({"schema_version":1,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85.5,"storage_usage_percent":90}})");
        auto settings = reader.read_settings();
        CHECK(settings);
        CHECK(settings->collection_interval == std::chrono::seconds{60});
        CHECK(settings->output_directory == "/Monitor");
        auto policy = reader.read();
        CHECK(policy);
        CHECK(settings->thresholds == *policy);
        CHECK(test::near(policy->cpu_usage_percent, 80));
        CHECK(test::near(policy->memory_usage_percent, 85.5));
        CHECK(test::near(policy->storage_usage_percent, 90));

        write_file(path, document_with_collection(R"({"interval_seconds":15,"output_directory":"/var/lib/resource-monitor snapshots"})"));
        settings = reader.read_settings();
        CHECK(settings);
        CHECK(settings->thresholds == *policy);
        CHECK(settings->collection_interval == std::chrono::seconds{15});
        CHECK(settings->output_directory == "/var/lib/resource-monitor snapshots");
        const auto full_policy = reader.read();
        CHECK(full_policy);
        CHECK(*full_policy == *policy);

        write_file(path, document_with_collection("{}"));
        settings = reader.read_settings();
        CHECK(settings);
        CHECK(settings->collection_interval == std::chrono::seconds{60});
        CHECK(settings->output_directory == "/Monitor");

        for (const auto interval : {1, 86400}) {
            write_file(path, document_with_collection("{\"interval_seconds\":" + std::to_string(interval) + "}"));
            settings = reader.read_settings();
            CHECK(settings);
            CHECK(settings->collection_interval == std::chrono::seconds{interval});
            CHECK(settings->output_directory == "/Monitor");
        }

        for (const auto& output : {std::string{"/"}, std::string{"/var/lib/resource-monitor"},
            std::string{"/var/lib/\uBAA8\uB2C8\uD130"}}) {
            write_file(path, document_with_collection("{\"output_directory\":\"" + output + "\"}"));
            settings = reader.read_settings();
            CHECK(settings);
            CHECK(settings->collection_interval == std::chrono::seconds{60});
            CHECK(settings->output_directory == output);
        }

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
            R"({"schema_version":1,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85,"storage_usage_percent":90},"collection":{},"collection":{}})",
            R"({"schema_version":1,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":101,"storage_usage_percent":90},"collection":{"interval_seconds":15,"output_directory":"/var/lib/monitor"}})",
            std::string(20, '[') + "0" + std::string(20, ']'),
            std::string(65537, ' ')
        };
        for (const auto& content : invalid) {
            write_file(path, content);
            check_failure(reader, path);
        }

        const std::vector<std::string> invalid_collection{
            "null", "true", "1", "\"settings\"", "[]",
            R"({"typo":60})",
            R"({"interval_seconds":60,"interval_seconds":30})",
            R"({"output_directory":"/Monitor","output_directory":"/var/lib/monitor"})",
            R"({"interval_seconds":0})",
            R"({"interval_seconds":-1})",
            R"({"interval_seconds":86401})",
            R"({"interval_seconds":-9223372036854775808})",
            R"({"interval_seconds":9223372036854775807})",
            R"({"interval_seconds":18446744073709551615})",
            R"({"interval_seconds":18446744073709551616})",
            R"({"interval_seconds":1e400})",
            R"({"interval_seconds":60.0})",
            R"({"interval_seconds":0.5})",
            R"({"interval_seconds":true})",
            R"({"interval_seconds":null})",
            R"({"interval_seconds":"60"})",
            R"({"interval_seconds":[]})",
            R"({"interval_seconds":{}})",
            R"({"output_directory":null})",
            R"({"output_directory":false})",
            R"({"output_directory":1})",
            R"({"output_directory":[]})",
            R"({"output_directory":{}})",
            R"({"output_directory":""})",
            R"({"output_directory":"Monitor"})",
            R"({"output_directory":"./Monitor"})",
            R"({"output_directory":"../Monitor"})",
            R"({"output_directory":"~/Monitor"})",
            R"({"output_directory":"C:\\Monitor"})",
            R"({"output_directory":"/Monitor\u0000hidden"})",
            R"({"output_directory":"/Monitor\nnext"})",
            R"({"output_directory":"/Monitor\rnext"})",
            R"({"output_directory":"/Monitor\tnext"})",
            R"({"output_directory":"/Monitor\u001fnext"})",
            R"({"output_directory":"/Monitor\u007fnext"})",
            R"({"output_directory":"/Monitor\u0080next"})",
            R"({"output_directory":"/Monitor\u009fnext"})",
            R"({"interval_seconds":15,"output_directory":"relative"})",
            R"({"interval_seconds":0,"output_directory":"/var/lib/monitor"})"
        };
        for (const auto& collection : invalid_collection) {
            write_file(path, document_with_collection(collection));
            check_failure(reader, path);
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

        monitor::JsonSettingsReader directory_reader(directory.path);
        check_failure(directory_reader, directory.path);
        const auto link = directory.path / "link.json";
        std::filesystem::create_symlink(path, link);
        monitor::JsonSettingsReader link_reader(link);
        check_failure(link_reader, link);
        const auto pipe = directory.path / "pipe.json";
        CHECK(::mkfifo(pipe.c_str(), 0600) == 0);
        monitor::JsonSettingsReader pipe_reader(pipe);
        check_failure(pipe_reader, pipe);
    });
}
