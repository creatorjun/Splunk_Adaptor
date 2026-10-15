// src/main.cpp
#include "application/CollectAndStore.hpp"
#include "infrastructure/config/JsonSettingsReader.hpp"
#include "infrastructure/linux/ProcResourceReader.hpp"
#include "infrastructure/persistence/JsonSnapshotStore.hpp"
#include "runtime/DaemonRunner.hpp"

#include <charconv>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    try {
        std::filesystem::path config_path{"/etc/resource-monitor/settings.json"};
        monitor::DaemonOptions options;
        bool check_config = false;
        bool output_overridden = false;
        bool interval_overridden = false;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument{argv[index]};
            if (argument == "--help") {
                std::cout << "Usage: resource-monitor [--config PATH] [--output PATH]\n"
                          << "                        [--interval-seconds 1..86400] [--once]\n"
                          << "                        [--check-config] [--version]\n"
                          << "Default config: /etc/resource-monitor/settings.json\n"
                          << "Explicit output and interval options override the configuration.\n"
                          << "First snapshot follows one full configured interval.\n";
                return 0;
            }
            if (argument == "--version") {
                std::cout << "resource-monitor 1.1.0\n";
                return 0;
            }
            if (argument == "--once") {
                options.once = true;
                continue;
            }
            if (argument == "--check-config") {
                check_config = true;
                continue;
            }
            if (argument != "--config" && argument != "--output" && argument != "--interval-seconds") {
                std::cerr << "Unknown argument: " << argument << '\n';
                return 64;
            }
            if (++index == argc) {
                std::cerr << "Missing value for " << argument << '\n';
                return 64;
            }
            const std::string_view value{argv[index]};
            if (value.empty()) {
                std::cerr << "Empty value for " << argument << '\n';
                return 64;
            }
            if (argument == "--config") {
                config_path = value;
            } else if (argument == "--output") {
                options.output_directory = value;
                output_overridden = true;
            } else {
                int seconds{};
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || seconds < 1 || seconds > 86400) {
                    std::cerr << "Interval must be an integer between 1 and 86400 seconds\n";
                    return 64;
                }
                options.interval = std::chrono::seconds{seconds};
                interval_overridden = true;
            }
        }
        monitor::JsonSettingsReader config{config_path};
        const auto settings = config.read_settings();
        if (!settings) {
            monitor::log_error(settings.error());
            return 78;
        }
        if (check_config) {
            std::cout << "Configuration is valid\n";
            return 0;
        }
        if (!output_overridden) {
            options.output_directory = settings->output_directory;
        }
        if (!interval_overridden) {
            options.interval = settings->collection_interval;
        }
        monitor::ProcResourceReader reader;
        monitor::JsonSnapshotStore store{options.output_directory};
        monitor::CollectAndStore use_case{reader, store, config};
        return monitor::run_daemon(use_case, options);
    } catch (const std::exception& error) {
        monitor::log_error({"runtime", "main", "unhandled_exception", error.what()});
        return 1;
    }
}
