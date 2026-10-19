// tests/JavaProcessMonitorTest.cpp
#include "TestSupport.hpp"
#include "application/JavaProcessMonitor.hpp"

#include <set>

namespace {

using namespace monitor;

struct Settings final : IMonitorSettingsProvider {
    MonitorSettings value;
    const MonitorSettings& current() const override { return value; }
};

struct Reader final : IJavaProcessReader {
    JavaProcessReading next;
    unsigned reads{};
    JavaProcessReading read() override {
        ++reads;
        return next;
    }
};

std::set<std::string> names(const JavaProcessSnapshot& snapshot) {
    std::set<std::string> result;
    for (const auto& [app, active] : snapshot.apps) {
        static_cast<void>(active);
        result.insert(app);
    }
    return result;
}

}

int main() {
    return test::run([] {
        const std::set<std::string> analyzer{
            "SearchEngine", "Normalizer", "CollectorForAgent", "CDMEngine", "ShovelServer",
            "AnalyzeEngine", "AnalyzeAssistantEngine", "SoarTicketMain", "SoarPlaybookManager",
            "pmsServer", "pmsClient", "ProcessMonitor"};
        const std::set<std::string> collector{
            "SearchEngine", "Reducer", "Normalizer", "CloudESMCollector", "CollectorForAgent",
            "CDMEngine", "pmsClient", "ProcessMonitor"};
        Settings settings;
        Reader reader;
        JavaProcessMonitor monitor{reader, settings};
        auto snapshot = monitor.collect();
        CHECK(reader.reads == 1);
        CHECK(!snapshot.is_analyzer);
        CHECK(names(snapshot) == collector);
        CHECK(snapshot.status == MetricStatus::warning);
        for (const auto& [app, active] : snapshot.apps) {
            static_cast<void>(app);
            CHECK(active == false);
        }

        reader.next.running_apps.assign(collector.begin(), collector.end());
        reader.next.running_apps.insert(reader.next.running_apps.end(), {
            "SearchEngine", "SearchEngine", "pmsServer", "", "UnknownApplication"});
        snapshot = monitor.collect();
        CHECK(snapshot.status == MetricStatus::ok);
        CHECK(snapshot.errors.empty());
        CHECK(names(snapshot) == collector);
        for (const auto& [app, active] : snapshot.apps) {
            static_cast<void>(app);
            CHECK(active == true);
        }

        settings.value.is_analyzer = true;
        snapshot = monitor.collect();
        CHECK(snapshot.is_analyzer);
        CHECK(names(snapshot) == analyzer);
        CHECK(snapshot.status == MetricStatus::warning);
        CHECK(snapshot.apps.at("pmsServer") == true);
        CHECK(snapshot.apps.at("AnalyzeEngine") == false);
        CHECK(!snapshot.apps.contains("Reducer"));
        CHECK(!snapshot.apps.contains("CloudESMCollector"));

        reader.next.running_apps.assign(analyzer.begin(), analyzer.end());
        snapshot = monitor.collect();
        CHECK(snapshot.status == MetricStatus::ok);
        CHECK(names(snapshot) == analyzer);
        for (const auto& [app, active] : snapshot.apps) {
            static_cast<void>(app);
            CHECK(active == true);
        }

        reader.next.errors.push_back(Error{"java_processes", "/proc/99/cmdline",
            "permission_denied", "Cannot inspect process arguments"});
        snapshot = monitor.collect();
        CHECK(snapshot.status == MetricStatus::unknown);
        CHECK(snapshot.apps.at("AnalyzeEngine") == true);
        CHECK(snapshot.errors.size() == 1);
        CHECK(snapshot.errors.front().code == "permission_denied");

        reader.next.running_apps = {"SearchEngine", "SearchEngine"};
        snapshot = monitor.collect();
        CHECK(snapshot.status == MetricStatus::unknown);
        CHECK(snapshot.apps.at("SearchEngine") == true);
        CHECK(!snapshot.apps.at("AnalyzeEngine").has_value());
        CHECK(names(snapshot) == analyzer);

        settings.value.is_analyzer = false;
        snapshot = monitor.collect();
        CHECK(!snapshot.is_analyzer);
        CHECK(names(snapshot) == collector);
        CHECK(snapshot.apps.at("SearchEngine") == true);
        CHECK(!snapshot.apps.at("Reducer").has_value());
        CHECK(snapshot.status == MetricStatus::unknown);

        reader.next.errors.clear();
        reader.next.running_apps = {"searchengine", "SearchEngineSuffix", "-DAPP=Reducer"};
        snapshot = monitor.collect();
        CHECK(snapshot.apps.at("SearchEngine") == false);
        CHECK(snapshot.apps.at("Reducer") == false);
        CHECK(snapshot.status == MetricStatus::warning);
        CHECK(snapshot.errors.empty());
        CHECK(reader.reads == 8);

        settings.value.process_monitor.collector = {"custom-collector", "case.Sensitive"};
        settings.value.process_monitor.analyzer = {"custom-analyzer", "Analyzer2"};
        reader.next.running_apps = {"custom-collector", "case.sensitive", "custom-analyzer"};
        snapshot = monitor.collect();
        CHECK(names(snapshot) == (std::set<std::string>{"custom-collector", "case.Sensitive"}));
        CHECK(snapshot.apps.at("custom-collector") == true);
        CHECK(snapshot.apps.at("case.Sensitive") == false);
        CHECK(snapshot.status == MetricStatus::warning);
        CHECK(!snapshot.apps.contains("SearchEngine"));

        settings.value.process_monitor.collector = {"custom-collector"};
        snapshot = monitor.collect();
        CHECK(names(snapshot) == (std::set<std::string>{"custom-collector"}));
        CHECK(snapshot.apps.at("custom-collector") == true);
        CHECK(snapshot.status == MetricStatus::ok);
        CHECK(!snapshot.apps.contains("case.Sensitive"));

        settings.value.process_monitor.analyzer.push_back("InactiveNewApp");
        snapshot = monitor.collect();
        CHECK(names(snapshot) == (std::set<std::string>{"custom-collector"}));
        CHECK(snapshot.status == MetricStatus::ok);

        settings.value.is_analyzer = true;
        snapshot = monitor.collect();
        CHECK(snapshot.is_analyzer);
        CHECK(names(snapshot) == (std::set<std::string>{"custom-analyzer", "Analyzer2", "InactiveNewApp"}));
        CHECK(snapshot.apps.at("custom-analyzer") == true);
        CHECK(snapshot.apps.at("Analyzer2") == false);
        CHECK(snapshot.apps.at("InactiveNewApp") == false);
        CHECK(snapshot.status == MetricStatus::warning);

        settings.value.process_monitor.analyzer = {"custom-analyzer"};
        snapshot = monitor.collect();
        CHECK(names(snapshot) == (std::set<std::string>{"custom-analyzer"}));
        CHECK(snapshot.status == MetricStatus::ok);
        CHECK(!snapshot.apps.contains("Analyzer2"));

        reader.next.errors.push_back(Error{"java_processes", "/proc", "permission_denied", "Access denied"});
        settings.value.process_monitor.analyzer.clear();
        const auto before_empty = reader.reads;
        snapshot = monitor.collect();
        CHECK(snapshot.is_analyzer);
        CHECK(snapshot.apps.empty());
        CHECK(snapshot.status == MetricStatus::ok);
        CHECK(snapshot.errors.empty());
        CHECK(reader.reads == before_empty);

        settings.value.is_analyzer = false;
        settings.value.process_monitor.collector.clear();
        snapshot = monitor.collect();
        CHECK(!snapshot.is_analyzer);
        CHECK(snapshot.apps.empty());
        CHECK(snapshot.status == MetricStatus::ok);
        CHECK(snapshot.errors.empty());
        CHECK(reader.reads == before_empty);

        settings.value.process_monitor.collector = {"RestoredApp"};
        snapshot = monitor.collect();
        CHECK(names(snapshot) == (std::set<std::string>{"RestoredApp"}));
        CHECK(!snapshot.apps.at("RestoredApp").has_value());
        CHECK(snapshot.status == MetricStatus::unknown);
        CHECK(snapshot.errors.size() == 1);
        CHECK(reader.reads == before_empty + 1);
    });
}
