// src/main/java/com/company/monitor/domain/MonitorSettings.java
package com.company.monitor.domain;

import java.util.List;
import java.util.Objects;

public record MonitorSettings(int sampleIntervalSeconds, int consecutiveSamples, int repeatIntervalSeconds,
                              ThresholdRule cpu, ThresholdRule memory, ThresholdRule disk, List<String> diskPaths) {
    public MonitorSettings {
        if (sampleIntervalSeconds < 1 || sampleIntervalSeconds > 300) {
            throw new IllegalArgumentException("측정 주기는 1~300초여야 합니다.");
        }
        if (consecutiveSamples < 1 || consecutiveSamples > 60) {
            throw new IllegalArgumentException("연속 도달 횟수는 1~60회여야 합니다.");
        }
        if (repeatIntervalSeconds < 0 || repeatIntervalSeconds > 86400) {
            throw new IllegalArgumentException("재알림 주기는 0~86400초여야 합니다. 0은 재알림 해제입니다.");
        }
        Objects.requireNonNull(cpu, "CPU 설정이 필요합니다.");
        Objects.requireNonNull(memory, "메모리 설정이 필요합니다.");
        Objects.requireNonNull(disk, "디스크 설정이 필요합니다.");
        if (diskPaths == null || diskPaths.isEmpty() || diskPaths.size() > 32) {
            throw new IllegalArgumentException("디스크 경로는 1~32개여야 합니다.");
        }
        diskPaths = List.copyOf(diskPaths);
        if (diskPaths.stream().distinct().count() != diskPaths.size()) {
            throw new IllegalArgumentException("디스크 경로가 중복되었습니다.");
        }
        for (String path : diskPaths) {
            if (!path.startsWith("/") || path.contains("\u0000") || path.contains("\\") || path.length() > 1024
                    || List.of(path.split("/", -1)).contains("..")) {
                throw new IllegalArgumentException("디스크 경로는 상위 이동 없는 Linux 절대 경로여야 합니다.");
            }
        }
    }

    public static MonitorSettings defaults() {
        return new MonitorSettings(5, 1, 300, new ThresholdRule(true, 80, 5),
                new ThresholdRule(true, 85, 5), new ThresholdRule(true, 90, 5), List.of("/"));
    }
}
