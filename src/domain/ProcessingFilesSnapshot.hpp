// src/domain/ProcessingFilesSnapshot.hpp
#pragma once

#include "domain/ResourceSample.hpp"

#include <string_view>

namespace monitor {

enum class ProcessingFilesStatus { disabled, pending, ok, partial, error, warning };

inline std::string_view to_string(ProcessingFilesStatus status) {
    switch (status) {
        case ProcessingFilesStatus::disabled: return "disabled";
        case ProcessingFilesStatus::pending: return "pending";
        case ProcessingFilesStatus::ok: return "ok";
        case ProcessingFilesStatus::partial: return "partial";
        case ProcessingFilesStatus::error: return "error";
        case ProcessingFilesStatus::warning: return "warning";
    }
    return "error";
}

struct ProcessingDirectoryCount {
    std::string path;
    std::optional<std::uint64_t> tmp_count;
    std::optional<std::uint64_t> done_count;
    std::vector<Error> errors;
};

struct ProcessingFileAlert {
    std::string path;
    std::string extension;
    std::uint64_t count{};
    std::uint64_t threshold{};
};

struct ProcessingFilesSnapshot {
    ProcessingFilesStatus status{ProcessingFilesStatus::disabled};
    std::uint32_t scan_interval_seconds{60};
    std::optional<std::chrono::system_clock::time_point> scan_started_at;
    std::optional<std::chrono::system_clock::time_point> scanned_at;
    std::vector<ProcessingDirectoryCount> directories;
    std::vector<ProcessingFileAlert> alerts;
};

}
