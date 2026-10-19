// src/main.cpp
#include "application/CollectAndStore.hpp"
#include "application/CollectCountMonitor.hpp"
#include "application/JavaProcessMonitor.hpp"
#include "application/LogSilenceMonitor.hpp"
#include "application/ProcessingFileMonitor.hpp"
#include "infrastructure/linux/DirectoryProcessingFileScanner.hpp"
#include "infrastructure/linux/ProcessingFileReader.hpp"
#include "infrastructure/linux/ProcJavaProcessReader.hpp"
#include "infrastructure/linux/LocalDayWindow.hpp"
#include "application/SettingsController.hpp"
#include "infrastructure/config/JsonSettingsReader.hpp"
#include "infrastructure/config/JsonMySqlCredentialsReader.hpp"
#include "infrastructure/database/MariaDbConnectionFactory.hpp"
#include "infrastructure/database/MySqlConfiguration.hpp"
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
                          << "Settings are reloaded every second without restarting.\n"
                          << "First snapshot follows one full configured interval.\n";
                return 0;
            }
            if (argument == "--version") {
                std::cout << "resource-monitor 1.8.0\n";
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
        monitor::JsonMySqlCredentialsReader credentials;
        if (check_config) {
            const auto parsed = config.read_settings();
            if (!parsed) {
                monitor::log_error(parsed.error());
                return 78;
            }
            if (parsed->is_analyzer) {
                const auto secret = credentials.read_credentials(parsed->mysql.credentials_file);
                if (!secret) {
                    monitor::log_error(secret.error());
                    return 78;
                }
            }
            std::cout << "Configuration is valid\n";
            return 0;
        }
        monitor::SettingsOverrides overrides;
        if (output_overridden) {
            overrides.output_directory = options.output_directory.string();
        }
        if (interval_overridden) {
            overrides.interval = options.interval;
        }
        monitor::JsonSnapshotStore store{options.output_directory};
        monitor::MariaDbConnectionFactory database_factory;
        monitor::MySqlConfiguration database{credentials, database_factory};
        monitor::SettingsController settings{config, store, overrides, &database};
        if (const auto initialized = settings.initialize(); !initialized) {
            monitor::log_error(initialized.error());
            return initialized.error().component == "settings_config" ? 78 : 1;
        }
        monitor::ProcResourceReader reader;
        monitor::LocalDayWindow calendar;
        monitor::CollectCountMonitor collect_count{database, settings, calendar};
        monitor::ProcJavaProcessReader process_reader;
        monitor::JavaProcessMonitor processes{process_reader, settings};
        monitor::LogSilenceMonitor log_silence{database, settings};
        monitor::DirectoryProcessingFileScanner file_scanner;
        monitor::ProcessingFileReader file_reader{file_scanner};
        monitor::ProcessingFileMonitor processing_files{file_reader, settings};
        monitor::CollectAndStore use_case{reader, store, settings, &collect_count, &processes, &log_silence,
            &processing_files};
        return monitor::run_daemon(use_case, options, &settings);
    } catch (const std::exception& error) {
        monitor::log_error({"runtime", "main", "unhandled_exception", error.what()});
        return 1;
    }
}
