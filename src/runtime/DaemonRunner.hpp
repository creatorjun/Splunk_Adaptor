// src/runtime/DaemonRunner.hpp
#pragma once

#include "application/CollectAndStore.hpp"

#include <chrono>
#include <filesystem>

namespace monitor {

struct DaemonOptions {
    std::filesystem::path output_directory{"/Monitor"};
    std::chrono::seconds interval{60};
    bool once{false};
};

int run_daemon(CollectAndStore& use_case, const DaemonOptions& options);
void log_error(const Error& error);

}
