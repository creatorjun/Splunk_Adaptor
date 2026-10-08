// src/main/java/com/company/monitor/domain/MinuteUsage.java
package com.company.monitor.domain;

import java.time.Instant;
import java.util.List;
import java.util.Objects;

public record MinuteUsage(Instant capturedAt, Double cpuPercent, Double memoryPercent, Double diskPercent,
                          List<String> errors) {
    public MinuteUsage {
        Objects.requireNonNull(capturedAt);
        errors = List.copyOf(errors);
        for (Double value : new Double[]{cpuPercent, memoryPercent, diskPercent}) {
            if (value != null && (!Double.isFinite(value) || value < 0 || value > 100)) {
                throw new IllegalArgumentException("사용률은 0~100% 범위여야 합니다.");
            }
        }
    }

    public static MinuteUsage from(ResourceSnapshot snapshot) {
        var maximum = snapshot.disks().stream().mapToDouble(ResourceSnapshot.DiskUsage::usedPercent).max();
        Double disk = maximum.isPresent() ? maximum.getAsDouble() : null;
        return new MinuteUsage(snapshot.capturedAt(), snapshot.cpuPercent(),
                snapshot.memory() == null ? null : snapshot.memory().usedPercent(), disk, snapshot.errors());
    }
}
