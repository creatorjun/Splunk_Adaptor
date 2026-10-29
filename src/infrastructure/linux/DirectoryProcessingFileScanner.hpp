// src/infrastructure/linux/DirectoryProcessingFileScanner.hpp
#pragma once

#include "application/ports/IProcessingFileScanner.hpp"

#include <filesystem>

namespace monitor {

struct ProcessingFileScanLimits {
    std::size_t max_depth{128};
    std::uint64_t max_entries{10000000};
    std::chrono::milliseconds max_duration{std::chrono::seconds{55}};
};

struct ProcessingDirectoryRoot {
    std::filesystem::path path;
    ProcessingDirectoryKind kind{ProcessingDirectoryKind::unknown};
};

class DirectoryProcessingFileScanner final : public IProcessingFileScanner {
public:
    DirectoryProcessingFileScanner();
    explicit DirectoryProcessingFileScanner(std::vector<ProcessingDirectoryRoot> roots,
        ProcessingFileScanLimits limits = {});
    ProcessingFilesSnapshot scan(std::stop_token stop) override;

private:
    std::vector<ProcessingDirectoryRoot> roots_;
    ProcessingFileScanLimits limits_;
};

}
