// tests/JsonSettingsReaderTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/config/JsonSettingsReader.hpp"

#include <nlohmann/json.hpp>

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

std::string document_with_mysql(const std::string& mysql, const std::string& analyzer = "true") {
    return R"({"schema_version":1,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85.5,"storage_usage_percent":90},"mysql":)"
        + mysql + (analyzer.empty() ? "" : ",\"role\":" + (analyzer == "true" ? "\"anal\"" : analyzer == "false" ? "\"col\"" : analyzer)) + "}";
}

std::string document_with_process_monitor(const std::string& processes, bool is_analyzer = false) {
    return R"({"schema_version":1,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85.5,"storage_usage_percent":90},"mysql":{"database":"monitoring","tls_mode":"disabled"},"role":)"
        + std::string{is_analyzer ? "\"anal\"" : "\"col\""} + ",\"process_monitor\":" + processes + "}";
}

std::string document_with_processing_files(const std::string& processing, bool is_analyzer = false) {
    return R"({"schema_version":1,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85.5,"storage_usage_percent":90},"mysql":{"database":"monitoring","tls_mode":"disabled"},"role":)"
        + std::string{is_analyzer ? "\"anal\"" : "\"col\""} + ",\"processing_files\":" + processing + "}";
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

void role_and_relay_settings_tests() {
    TemporaryDirectory directory;
    const auto path = directory.path / "settings.json";
    monitor::JsonSettingsReader reader(path);
    const auto base = nlohmann::json::parse(document_with_collection("{}"));
    for (const auto role : {monitor::ServerRole::col, monitor::ServerRole::anal, monitor::ServerRole::prx}) {
        auto document = base;
        document["role"] = monitor::to_string(role);
        document["mysql"] = {{"database", "monitoring"}, {"tls_mode", "disabled"}};
        document["process_monitor"] = {{"prx", {"RelayCustom", "CollectorForAgent"}}};
        document["relay_log"] = {{"path", "/custom/missing-relay.log"}};
        write_file(path, document.dump());
        auto settings = reader.read_settings();
        CHECK(settings && settings->role == role);
        CHECK(settings->relay_log.path == "/custom/missing-relay.log");
        CHECK(settings->process_monitor.prx == (std::vector<std::string>{"RelayCustom", "CollectorForAgent"}));
        for (const bool legacy : {false, true}) {
            document["is_analyzer"] = legacy;
            write_file(path, document.dump());
            const auto rejected = reader.read_settings();
            CHECK(!rejected && rejected.error().code == "invalid_role");
        }
        document.erase("is_analyzer");
        document.erase("mysql");
        write_file(path, document.dump());
        settings = reader.read_settings();
        CHECK(settings.has_value() == (role != monitor::ServerRole::anal));
        if (!settings) CHECK(settings.error().code == "invalid_mysql");
    }
    for (const auto* invalid : {"null", "false", "true", "0", "1", "[]", "{}", "\"\"",
        "\"Analyzer\"", "\"collector\"", "\"analyzer\"", "\"relay\"", "\"relay \"", "\"unknown\"", "\"redaction-marker\""}) {
        auto document = base;
        document["role"] = nlohmann::json::parse(invalid);
        write_file(path, document.dump());
        const auto rejected = reader.read_settings();
        CHECK(!rejected && rejected.error().code == "invalid_role");
        CHECK(rejected.error().message.find("redaction-marker") == std::string::npos);
    }
    for (const auto* invalid : {"null", "false", "1", "[]", "\"file\"", R"({"unknown":1})",
        R"({"path":null})", R"({"path":1})", R"({"path":[]})", R"({"path":""})",
        R"({"path":"relative.log"})", R"({"path":"/"})", R"({"path":"/logs/"})",
        R"({"path":"/logs/."})", R"({"path":"/logs/.."})", R"({"path":"/logs/redaction-marker\n.log"})",
        R"({"path":"/logs/redaction-marker\u0000.log"})", R"({"path":"/logs/redaction-marker\u0085.log"})"}) {
        auto document = base;
        document["role"] = "prx";
        document["relay_log"] = nlohmann::json::parse(invalid);
        write_file(path, document.dump());
        const auto rejected = reader.read_settings();
        CHECK(!rejected && rejected.error().code == "invalid_relay_log");
        CHECK(rejected.error().message.find("redaction-marker") == std::string::npos);
    }
    for (const auto* legacy_profile : {"collector", "analyzer", "relay"}) {
        auto legacy_document = base;
        legacy_document["process_monitor"] = {{legacy_profile, nlohmann::json::array()}};
        write_file(path, legacy_document.dump());
        const auto rejected = reader.read_settings();
        CHECK(!rejected && rejected.error().code == "invalid_process_monitor");
    }
    auto document = base;
    document["role"] = "prx";
    document["relay_log"] = nlohmann::json::object();
    document["process_monitor"] = {{"prx", nlohmann::json::array()}};
    write_file(path, document.dump());
    auto settings = reader.read_settings();
    CHECK(settings && settings->relay_log == monitor::RelayLogSettings{});
    CHECK(settings->process_monitor.prx.empty());
    document.erase("process_monitor");
    write_file(path, document.dump());
    settings = reader.read_settings();
    CHECK(settings && settings->process_monitor.prx == (std::vector<std::string>{"CollectorForAgent"}));
    for (const auto* invalid : {R"({"role":"col","role":"prx"})",
        R"({"relay_log":{"path":"/a.log","path":"/b.log"}})",
        R"({"process_monitor":{"prx":[],"prx":[]}})"}) {
        auto fragment = std::string{invalid};
        fragment.erase(0, 1);
        auto text = base.dump();
        text.pop_back();
        write_file(path, text + "," + fragment);
        CHECK(!reader.read_settings());
    }
}

void processing_files_settings_tests() {
    TemporaryDirectory directory;
    const auto path = directory.path / "settings.json";
    monitor::JsonSettingsReader reader(path);
    for (const auto* partial : {"{}", R"({"thresholds":{}})",
        R"({"thresholds":{"collector":{},"normalize":{}}})"}) {
        write_file(path, document_with_processing_files(partial));
        const auto settings = reader.read_settings();
        CHECK(settings);
        CHECK(settings->processing_files == monitor::ProcessingFileSettings{});
    }
    for (const bool analyzer : {false, true}) {
        write_file(path, document_with_processing_files(
            R"({"thresholds":{"collector":{"tmp_count":0,"done_count":1000000000000},"normalize":{"tmp_count":101,"done_count":99}}})", analyzer));
        const auto settings = reader.read_settings();
        CHECK(settings);
        CHECK(settings->role == (analyzer ? monitor::ServerRole::anal : monitor::ServerRole::col));
        CHECK(settings->processing_files.collector.tmp_count == 0);
        CHECK(settings->processing_files.collector.done_count == 1000000000000ULL);
        CHECK(settings->processing_files.normalize.tmp_count == 101);
        CHECK(settings->processing_files.normalize.done_count == 99);
    }
    for (const auto* profile : {"collector", "normalize"}) {
        for (const auto* extension : {"tmp_count", "done_count"}) {
            write_file(path, document_with_processing_files(nlohmann::json{
                {"thresholds", {{profile, {{extension, 42}}}}}}.dump()));
            const auto settings = reader.read_settings();
            CHECK(settings);
            const bool collector = std::string_view{profile} == "collector";
            const auto& changed = collector ? settings->processing_files.collector : settings->processing_files.normalize;
            const auto& unchanged = collector ? settings->processing_files.normalize : settings->processing_files.collector;
            CHECK(unchanged == monitor::ProcessingFileThresholds{});
            CHECK(changed.tmp_count == (std::string_view{extension} == "tmp_count" ? 42ULL : 100ULL));
            CHECK(changed.done_count == (std::string_view{extension} == "done_count" ? 42ULL : 100ULL));
            for (const auto* invalid : {"-1", "1000000000001", "18446744073709551615", "18446744073709551616",
                "-9223372036854775808", "100.0", "1e2", "0.5", "true", "false", "null", "[]", "{}", "\"redaction-marker\""}) {
                write_file(path, document_with_processing_files(
                    "{\"thresholds\":{\"" + std::string{profile} + "\":{\"" + extension + "\":" + invalid + "}}}"));
                const auto rejected = reader.read_settings();
                CHECK(!rejected);
                CHECK(rejected.error().code == "invalid_processing_files");
                CHECK(rejected.error().message.find("redaction-marker") == std::string::npos);
            }
        }
    }
    for (const auto* invalid : {"null", "true", "100", "[]", "\"redaction-marker\"",
        R"({"enabled":true})", R"({"scan_interval_seconds":30})", R"({"redaction-marker":100})",
        R"({"thresholds":null})", R"({"thresholds":[]})", R"({"thresholds":{"redaction-marker":{}}})",
        R"({"thresholds":{"collector":null}})", R"({"thresholds":{"normalize":[]}})",
        R"({"thresholds":{"collector":{"redaction-marker":100}}})",
        R"({"thresholds":{"normalize":{"path":"redaction-marker"}}})"}) {
        write_file(path, document_with_processing_files(invalid));
        const auto rejected = reader.read_settings();
        CHECK(!rejected);
        CHECK(rejected.error().code == "invalid_processing_files");
        CHECK(rejected.error().message.find("redaction-marker") == std::string::npos);
    }
    for (const auto* invalid : {
        R"({"thresholds":{"collector":{"tmp_count":100,"tmp_count":200}}})",
        R"({"thresholds":{"collector":{},"collector":{}}})",
        R"({"thresholds":{},"thresholds":{}})"}) {
        write_file(path, document_with_processing_files(invalid));
        check_failure(reader, path);
    }
}

void retention_settings_tests() {
    TemporaryDirectory directory;
    const auto path = directory.path / "settings.json";
    monitor::JsonSettingsReader reader(path);
    const auto document = [](const std::string& retention) {
        return R"({"schema_version":1,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85,"storage_usage_percent":90},"retention":)"
            + retention + "}";
    };
    for (const auto* value : {"{}", R"({"enabled":true})", R"({"max_age_seconds":3600})"}) {
        write_file(path, document(value));
        const auto settings = reader.read_settings();
        CHECK(settings && settings->retention == monitor::SnapshotRetentionSettings{});
    }
    for (const bool enabled : {false, true}) {
        for (const auto seconds : {60, 7200, 31536000}) {
            write_file(path, document(nlohmann::json{
                {"enabled", enabled}, {"max_age_seconds", seconds}}.dump()));
            const auto settings = reader.read_settings();
            CHECK(settings);
            CHECK(settings->retention.enabled == enabled);
            CHECK(settings->retention.max_age == std::chrono::seconds{seconds});
        }
    }
    for (const auto* invalid : {"null", "true", "[]", "3600", "\"redaction-marker\"",
        R"({"enabled":1})", R"({"enabled":null})", R"({"enabled":"false"})",
        R"({"max_age_seconds":0})", R"({"max_age_seconds":-1})", R"({"max_age_seconds":59})",
        R"({"max_age_seconds":31536001})", R"({"max_age_seconds":18446744073709551615})",
        R"({"max_age_seconds":18446744073709551616})", R"({"max_age_seconds":-9223372036854775808})",
        R"({"max_age_seconds":3600.0})", R"({"max_age_seconds":true})", R"({"max_age_seconds":"3600"})",
        R"({"max_age_seconds":null})", R"({"enabled":false,"max_age_seconds":0})",
        R"({"redaction-marker":1})"}) {
        write_file(path, document(invalid));
        const auto rejected = reader.read_settings();
        CHECK(!rejected);
        CHECK(rejected.error().code == "invalid_retention");
        CHECK(rejected.error().message.find("redaction-marker") == std::string::npos);
    }
    write_file(path, document(R"({"enabled":false,"enabled":true})"));
    check_failure(reader, path);
}

void cached_settings_preserve_reload_and_file_checks() {
    TemporaryDirectory directory;
    const auto path = directory.path / "settings.json";
    const auto original = document_with_collection(R"({"interval_seconds":60,"output_directory":"/Monitor"})");
    write_file(path, original);
    monitor::JsonSettingsReader reader(path);
    auto first = reader.read_settings();
    CHECK(first && first->thresholds.cpu_usage_percent == 80);
    first->thresholds.cpu_usage_percent = 1;
    CHECK(reader.read_settings()->thresholds.cpu_usage_percent == 80);
    const auto modified_at = std::filesystem::last_write_time(path);
    auto changed = original;
    const auto value = changed.find("\"cpu_usage_percent\":80");
    CHECK(value != std::string::npos);
    changed.replace(value, std::string{"\"cpu_usage_percent\":80"}.size(), "\"cpu_usage_percent\":81");
    CHECK(changed.size() == original.size());
    write_file(path, changed);
    std::filesystem::last_write_time(path, modified_at);
    CHECK(reader.read_settings()->thresholds.cpu_usage_percent == 81);
    write_file(path, "{");
    CHECK(!reader.read_settings());
    write_file(path, changed);
    CHECK(reader.read_settings()->thresholds.cpu_usage_percent == 81);
    const auto saved = directory.path / "saved.json";
    std::filesystem::rename(path, saved);
    CHECK(!reader.read_settings());
    CHECK(::mkfifo(path.c_str(), 0600) == 0);
    CHECK(!reader.read_settings());
    std::filesystem::remove(path);
    std::filesystem::create_symlink(saved, path);
    CHECK(!reader.read_settings());
    std::filesystem::remove(path);
    std::filesystem::rename(saved, path);
    CHECK(reader.read_settings()->thresholds.cpu_usage_percent == 81);
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
        CHECK(settings->mysql == monitor::MySqlSettings{});
        CHECK(settings->role == monitor::ServerRole::col);
        const std::vector<std::string> default_analyzer{
            "SearchEngine", "Normalizer", "CollectorForAgent", "CDMEngine", "ShovelServer",
            "AnalyzeEngine", "AnalyzeAssistantEngine", "SoarTicketMain", "SoarPlaybookManager",
            "pmsServer", "pmsClient", "ProcessMonitor"};
        const std::vector<std::string> default_collector{
            "SearchEngine", "Reducer", "Normalizer", "CloudESMCollector", "CollectorForAgent",
            "CDMEngine", "pmsClient", "ProcessMonitor"};
        CHECK(settings->process_monitor.anal == default_analyzer);
        CHECK(settings->process_monitor.col == default_collector);
        CHECK(settings->processing_files == monitor::ProcessingFileSettings{});
        CHECK(settings->retention == monitor::SnapshotRetentionSettings{});
        auto policy = reader.read();
        CHECK(policy);
        CHECK(settings->thresholds == *policy);
        CHECK(test::near(policy->cpu_usage_percent, 80));
        CHECK(test::near(policy->memory_usage_percent, 85.5));
        CHECK(test::near(policy->storage_usage_percent, 90));

        write_file(path, document_with_process_monitor("{}"));
        settings = reader.read_settings();
        CHECK(settings);
        CHECK(settings->process_monitor.anal == default_analyzer);
        CHECK(settings->process_monitor.col == default_collector);
        const std::vector<std::string> custom_analyzer{"SearchEngine", "searchengine", "Custom App", "\uBD84\uC11D\uAE30"};
        const std::vector<std::string> custom_collector{"Reducer", "Collector v2"};
        write_file(path, document_with_process_monitor(nlohmann::json{
            {"anal", custom_analyzer}, {"col", custom_collector}}.dump(), true));
        settings = reader.read_settings();
        CHECK(settings);
        CHECK(settings->role == monitor::ServerRole::anal);
        CHECK(settings->process_monitor.anal == custom_analyzer);
        CHECK(settings->process_monitor.col == custom_collector);
        CHECK(settings->thresholds == *policy);
        for (const auto* profile : {"anal", "col"}) {
            write_file(path, document_with_process_monitor(nlohmann::json{
                {profile, std::vector<std::string>{"OnlyThisApp"}}}.dump()));
            settings = reader.read_settings();
            CHECK(settings);
            const bool analyzer = std::string_view{profile} == "anal";
            const auto& customized = analyzer
                ? settings->process_monitor.anal : settings->process_monitor.col;
            CHECK(customized.size() == 1);
            CHECK(customized.front() == "OnlyThisApp");
            CHECK((analyzer ? settings->process_monitor.col : settings->process_monitor.anal)
                == (analyzer ? default_collector : default_analyzer));
            write_file(path, document_with_process_monitor(nlohmann::json{
                {profile, nlohmann::json::array()}}.dump()));
            settings = reader.read_settings();
            CHECK(settings);
            CHECK((analyzer ? settings->process_monitor.anal : settings->process_monitor.col).empty());
            CHECK((analyzer ? settings->process_monitor.col : settings->process_monitor.anal)
                == (analyzer ? default_collector : default_analyzer));
        }
        write_file(path, document_with_process_monitor(R"({"anal":[],"col":[]})"));
        settings = reader.read_settings();
        CHECK(settings);
        CHECK(settings->process_monitor.anal.empty());
        CHECK(settings->process_monitor.col.empty());

        std::string multibyte_limit;
        for (int index = 0; index < 64; ++index) multibyte_limit += "\xc3\xa9";
        const std::vector<std::string> length_boundaries{"A", std::string(128, 'X'), multibyte_limit};
        write_file(path, document_with_process_monitor(nlohmann::json{
            {"anal", length_boundaries}, {"col", length_boundaries}}.dump()));
        settings = reader.read_settings();
        CHECK(settings);
        CHECK(settings->process_monitor.anal == length_boundaries);
        CHECK(settings->process_monitor.col == length_boundaries);
        nlohmann::json maximum_apps = nlohmann::json::array();
        for (int index = 0; index < 256; ++index) maximum_apps.push_back("App" + std::to_string(index));
        write_file(path, document_with_process_monitor(nlohmann::json{
            {"anal", maximum_apps}, {"col", maximum_apps}}.dump()));
        settings = reader.read_settings();
        CHECK(settings);
        CHECK(settings->process_monitor.anal.size() == 256);
        CHECK(settings->process_monitor.col.size() == 256);
        CHECK(settings->process_monitor.anal.front() == "App0");
        CHECK(settings->process_monitor.anal.back() == "App255");
        maximum_apps.push_back("App256");
        const std::vector<std::string> invalid_process_lists{
            "null", "true", "1", "\"redaction-marker\"", "{}",
            "[null]", "[false]", "[1]", "[{}]", "[[]]", "[\"\"]",
            R"(["redaction-marker","redaction-marker"])",
            R"([" redaction-marker"])", R"(["redaction-marker "])", R"([" "])",
            R"(["redaction-marker\t"])", R"(["\rredaction-marker"])",
            R"(["redaction\nmarker"])", R"(["redaction\fmarker"])",
            R"(["redaction\u0000marker"])", R"(["redaction\u000bmarker"])",
            R"(["redaction\u001fmarker"])", R"(["redaction\u007fmarker"])",
            R"(["redaction\u0080marker"])", R"(["redaction\u009fmarker"])",
            nlohmann::json(std::vector<std::string>{std::string(129, 'X')}).dump(),
            nlohmann::json(std::vector<std::string>{multibyte_limit + "X"}).dump(),
            maximum_apps.dump()
        };
        for (const auto is_analyzer : {false, true}) {
            for (const auto* profile : {"anal", "col", "prx"}) {
                for (const auto& apps : invalid_process_lists) {
                    write_file(path, document_with_process_monitor(
                        "{\"" + std::string{profile} + "\":" + apps + "}", is_analyzer));
                    const auto result = reader.read_settings();
                    CHECK(!result);
                    CHECK(result.error().code == "invalid_process_monitor");
                    CHECK(result.error().message.find("redaction-marker") == std::string::npos);
                    check_failure(reader, path);
                }
            }
        }
        for (const auto& processes : {"null", "true", "1", "\"redaction-marker\"", "[]",
            R"({"redaction-marker":[]})"}) {
            write_file(path, document_with_process_monitor(processes));
            const auto result = reader.read_settings();
            CHECK(!result);
            CHECK(result.error().code == "invalid_process_monitor");
            CHECK(result.error().message.find("redaction-marker") == std::string::npos);
        }
        write_file(path, document_with_process_monitor(R"({"anal":[],"anal":["DuplicateKey"]})"));
        check_failure(reader, path);

        for (const auto* obsolete : {"analyzer", "log_silence", "log_volume"}) {
            auto document = nlohmann::json::parse(document_with_collection("{}"));
            document[obsolete] = nlohmann::json::object();
            write_file(path, document.dump());
            check_failure(reader, path);
        }

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

        write_file(path, document_with_mysql("{}", "false"));
        settings = reader.read_settings();
        CHECK(settings);
        CHECK(settings->mysql == monitor::MySqlSettings{});
        write_file(path, document_with_mysql(R"({"host":"db.internal","port":3307,"database":"monitoring","credentials_file":"/etc/resource-monitor/secrets/custom.json","connect_timeout_seconds":1,"read_timeout_seconds":5,"write_timeout_seconds":10,"tls_mode":"verify_identity","tls_ca_file":"/etc/pki/db-ca.pem"})"));
        settings = reader.read_settings();
        CHECK(settings);
        CHECK(settings->role == monitor::ServerRole::anal);
        CHECK(settings->mysql.host == "db.internal");
        CHECK(settings->mysql.port == 3307);
        CHECK(settings->mysql.database == "monitoring");
        CHECK(settings->mysql.credentials_file == "/etc/resource-monitor/secrets/custom.json");
        CHECK(settings->mysql.connect_timeout_seconds == 1);
        CHECK(settings->mysql.read_timeout_seconds == 5);
        CHECK(settings->mysql.write_timeout_seconds == 10);
        CHECK(settings->mysql.tls_mode == "verify_identity");
        CHECK(settings->mysql.tls_ca_file == "/etc/pki/db-ca.pem");
        for (const auto& analyzer : {std::string{}, std::string{"false"}}) {
            for (const auto& mysql : {
                std::string{"{}"},
                std::string{R"({"database":"","tls_ca_file":""})"},
                std::string{R"({"database":"monitoring","tls_mode":"verify_identity"})"}
            }) {
                write_file(path, document_with_mysql(mysql, analyzer));
                settings = reader.read_settings();
                CHECK(settings);
                CHECK(settings->role == monitor::ServerRole::col);
            }
        }
        for (const auto& analyzer : {"null", "1", "0", "1.0", "\"true\"", "\"false\"", "[]", "{}"}) {
            write_file(path, document_with_mysql("{}", analyzer));
            check_failure(reader, path);
        }
        write_file(path, document_with_mysql("{}", "false,\"is_analyzer\":true"));
        check_failure(reader, path);
        for (const auto& analyzer : {std::string{}, std::string{"false"}, std::string{"true"}}) {
            for (const auto& enabled : {"false", "true", "null", "1", "\"true\""}) {
                write_file(path, document_with_mysql(
                    R"({"database":"monitoring","tls_mode":"disabled","enabled":)"
                    + std::string{enabled} + "}", analyzer));
                check_failure(reader, path);
            }
        }
        for (const auto& mysql : {
            std::string{R"({"port":1,"database":"monitoring","tls_mode":"disabled"})"},
            std::string{R"({"port":65535,"database":"monitoring","tls_mode":"verify_identity","tls_ca_file":"/etc/pki/db-ca.pem"})"},
            std::string{R"({"database":"monitoring","tls_mode":"disabled"})"},
            std::string{R"({"database":"monitoring","tls_ca_file":"/etc/pki/db-ca.pem"})"}
        }) {
            write_file(path, document_with_mysql(mysql));
            CHECK(reader.read_settings());
        }
        write_file(path, R"({"schema_version":1,"is_analyzer":true,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85,"storage_usage_percent":90}})");
        check_failure(reader, path);
        for (const auto& mysql : {
            std::string{"{}"},
            std::string{R"({"database":"","tls_mode":"disabled"})"},
            std::string{R"({"database":"monitoring"})"},
            std::string{R"({"database":"monitoring","tls_mode":"verify_identity","tls_ca_file":""})"}
        }) {
            write_file(path, document_with_mysql(mysql));
            check_failure(reader, path);
        }
        const std::vector<std::string> invalid_mysql{
            "null", "true", "1", "\"db\"", "[]",
            R"({"host":""})", R"({"host":1})",
            R"({"host":"db\u0000host"})", R"({"host":"db\nhost"})",
            R"({"database":null})", R"({"database":"db\u007f"})",
            R"({"port":0})", R"({"port":-1})", R"({"port":65536})", R"({"port":3306.0})",
            R"({"port":true})", R"({"port":"3306"})", R"({"port":18446744073709551615})",
            R"({"port":18446744073709551616})", R"({"port":-9223372036854775808})",
            R"({"connect_timeout_seconds":0})", R"({"connect_timeout_seconds":11})",
            R"({"read_timeout_seconds":-1})", R"({"read_timeout_seconds":2.5})",
            R"({"write_timeout_seconds":"3"})", R"({"write_timeout_seconds":null})",
            R"({"credentials_file":""})", R"({"credentials_file":"relative.json"})",
            R"({"credentials_file":"/tmp/a\u0000.json"})", R"({"credentials_file":[]})",
            R"({"tls_mode":"preferred"})", R"({"tls_mode":"required"})",
            R"({"tls_mode":true})", R"({"tls_ca_file":"relative.pem"})",
            R"({"tls_ca_file":"/tmp/ca\n.pem"})", R"({"tls_ca_file":null})",
            R"({"username":"redaction-marker"})", R"({"password":"redaction-marker"})",
            R"({"redaction-marker":"value"})", R"({"host":"db","host":"db2"})"
        };
        for (const auto& mysql : invalid_mysql) {
            for (const auto& analyzer : {"false", "true"}) {
                write_file(path, document_with_mysql(mysql, analyzer));
                check_failure(reader, path);
                CHECK(reader.read_settings().error().message.find("redaction-marker") == std::string::npos);
            }
        }
        for (const auto& content : {
            document_with_mysql(R"({"password":"redaction-marker" INVALID})"),
            std::string{R"({"schema_version":1,"thresholds":{"cpu_usage_percent":80,"memory_usage_percent":85,"storage_usage_percent":90},"redaction-marker":true})"},
            document_with_collection(R"({"redaction-marker":true})")
        }) {
            write_file(path, content);
            const auto result = reader.read_settings();
            CHECK(!result);
            CHECK(result.error().message.find("redaction-marker") == std::string::npos);
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
        processing_files_settings_tests();
        retention_settings_tests();
        role_and_relay_settings_tests();
        cached_settings_preserve_reload_and_file_checks();
    });
}
