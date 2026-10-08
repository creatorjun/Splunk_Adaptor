// src/main/java/com/company/monitor/domain/ResourceSnapshot.java
package com.company.monitor.domain;

import java.time.Instant;
import java.util.List;

public record ResourceSnapshot(Instant capturedAt, String host, Double cpuPercent, MemoryUsage memory,
                               List<DiskUsage> disks, List<String> errors) {
    public ResourceSnapshot {
        disks = List.copyOf(disks);
        errors = List.copyOf(errors);
    }

    public record MemoryUsage(long totalBytes, long availableBytes, long usedBytes, double usedPercent) { }

    public record DiskUsage(String path, long totalBytes, long freeBytes, long usedBytes, double usedPercent) { }
}
