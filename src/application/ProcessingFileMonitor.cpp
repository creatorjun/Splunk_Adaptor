// src/application/ProcessingFileMonitor.cpp
#include "application/ProcessingFileMonitor.hpp"

namespace monitor {

ProcessingFileMonitor::ProcessingFileMonitor(IProcessingFileReader& reader,
    const IMonitorSettingsProvider& settings) : reader_(reader), settings_(settings) {}

ProcessingFilesSnapshot ProcessingFileMonitor::collect() {
    const auto& settings = settings_.current();
    const bool enabled = settings.role == ServerRole::col;
    auto snapshot = reader_.read(enabled);
    if (!enabled) return {};
    snapshot.alerts.clear();
    if (snapshot.status == ProcessingFilesStatus::pending ||
        snapshot.status == ProcessingFilesStatus::disabled) return snapshot;
    for (const auto& directory : snapshot.directories) {
        if (!directory.errors.empty()) continue;
        const ProcessingFileThresholds* thresholds = nullptr;
        if (directory.kind == ProcessingDirectoryKind::collector) {
            thresholds = &settings.processing_files.collector;
        } else if (directory.kind == ProcessingDirectoryKind::normalize) {
            thresholds = &settings.processing_files.normalize;
        }
        if (!thresholds) continue;
        if (directory.tmp_count && *directory.tmp_count >= thresholds->tmp_count) {
            snapshot.alerts.push_back({directory.path, ".tmp", *directory.tmp_count,
                thresholds->tmp_count});
        }
        if (directory.done_count && *directory.done_count >= thresholds->done_count) {
            snapshot.alerts.push_back({directory.path, ".done", *directory.done_count,
                thresholds->done_count});
        }
    }
    if (!snapshot.alerts.empty()) snapshot.status = ProcessingFilesStatus::warning;
    return snapshot;
}

void ProcessingFileMonitor::settings_changed() {
    reader_.read(settings_.current().role == ServerRole::col);
}

}
