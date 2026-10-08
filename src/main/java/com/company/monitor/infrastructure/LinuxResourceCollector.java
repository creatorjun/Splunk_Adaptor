// src/main/java/com/company/monitor/infrastructure/LinuxResourceCollector.java
package com.company.monitor.infrastructure;

import com.company.monitor.application.MonitorPorts;
import com.company.monitor.domain.MonitorSettings;
import com.company.monitor.domain.ResourceSnapshot;
import java.io.IOException;
import java.nio.file.FileStore;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class LinuxResourceCollector implements MonitorPorts.Collector {
    private record CpuTicks(long total, long idle) { }

    private final Path procRoot;
    private final Path diskRoot;
    private final String hostname;
    private CpuTicks previousCpu;

    public LinuxResourceCollector(Path procRoot, Path diskRoot, String hostname) {
        this.procRoot = procRoot;
        this.diskRoot = diskRoot.toAbsolutePath().normalize();
        this.hostname = hostname;
    }

    @Override
    public ResourceSnapshot collect(MonitorSettings settings, Instant at) {
        List<String> errors = new ArrayList<>();
        Double cpu = null;
        ResourceSnapshot.MemoryUsage memory = null;
        List<ResourceSnapshot.DiskUsage> disks = new ArrayList<>();
        try {
            cpu = readCpu();
        } catch (Exception exception) {
            previousCpu = null;
            errors.add("CPU: " + exception.getMessage());
        }
        try {
            memory = readMemory();
        } catch (Exception exception) {
            errors.add("Memory: " + exception.getMessage());
        }
        for (String path : settings.diskPaths()) {
            try {
                disks.add(readDisk(path));
            } catch (Exception exception) {
                errors.add("Disk " + path + ": " + exception.getMessage());
            }
        }
        return new ResourceSnapshot(at, hostname, cpu, memory, disks, errors);
    }

    private Double readCpu() throws IOException {
        String line;
        try (var reader = Files.newBufferedReader(procRoot.resolve("stat"))) {
            line = reader.readLine();
        }
        if (line == null || !line.startsWith("cpu ")) {
            throw new IOException("/proc/stat의 전체 CPU 행을 읽을 수 없습니다.");
        }
        String[] values = line.trim().split("\\s+");
        if (values.length < 5) {
            throw new IOException("CPU 카운터가 부족합니다.");
        }
        long total = 0;
        long idle = 0;
        for (int index = 1; index <= Math.min(8, values.length - 1); index++) {
            long ticks = Long.parseLong(values[index]);
            if (ticks < 0) {
                throw new IOException("CPU 카운터가 음수입니다.");
            }
            total = Math.addExact(total, ticks);
            if (index == 4 || index == 5) {
                idle = Math.addExact(idle, ticks);
            }
        }
        CpuTicks current = new CpuTicks(total, idle);
        CpuTicks previous = previousCpu;
        previousCpu = current;
        if (previous == null) {
            return null;
        }
        long totalDelta = current.total() - previous.total();
        long idleDelta = current.idle() - previous.idle();
        if (totalDelta <= 0 || idleDelta < 0 || idleDelta > totalDelta) {
            throw new IOException("CPU 카운터가 재설정되었거나 유효한 측정 간격이 없습니다.");
        }
        return 100.0 * (totalDelta - idleDelta) / totalDelta;
    }

    private ResourceSnapshot.MemoryUsage readMemory() throws IOException {
        Map<String, Long> values = new HashMap<>();
        for (String line : Files.readAllLines(procRoot.resolve("meminfo"))) {
            String[] parts = line.trim().split("\\s+");
            if (parts.length >= 2 && parts[0].endsWith(":")) {
                String key = parts[0].substring(0, parts[0].length() - 1);
                values.put(key, Long.parseLong(parts[1]));
            }
        }
        Long totalKb = values.get("MemTotal");
        Long availableKb = values.get("MemAvailable");
        if (totalKb == null || availableKb == null || totalKb <= 0 || availableKb < 0 || availableKb > totalKb) {
            throw new IOException("MemTotal/MemAvailable 값이 유효하지 않습니다.");
        }
        long total = Math.multiplyExact(totalKb, 1024);
        long available = Math.multiplyExact(availableKb, 1024);
        return new ResourceSnapshot.MemoryUsage(total, available, total - available, 100.0 * (total - available) / total);
    }

    private ResourceSnapshot.DiskUsage readDisk(String hostPath) throws IOException {
        Path path = diskRoot.resolve(hostPath.substring(1)).normalize();
        Path realRoot = diskRoot.toRealPath();
        Path realPath = path.toRealPath();
        if (!realPath.startsWith(realRoot)) {
            throw new IOException("호스트 마운트 범위 밖의 심볼릭 링크입니다.");
        }
        FileStore store = Files.getFileStore(realPath);
        long total = store.getTotalSpace();
        long free = store.getUnallocatedSpace();
        if (total <= 0 || free < 0 || free > total) {
            throw new IOException("파일시스템 용량을 측정할 수 없습니다.");
        }
        long used = total - free;
        return new ResourceSnapshot.DiskUsage(hostPath, total, free, used, 100.0 * used / total);
    }
}
