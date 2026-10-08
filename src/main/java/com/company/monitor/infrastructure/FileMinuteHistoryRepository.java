// src/main/java/com/company/monitor/infrastructure/FileMinuteHistoryRepository.java
package com.company.monitor.infrastructure;

import com.company.monitor.application.MonitorPorts;
import com.company.monitor.domain.MinuteUsage;
import com.fasterxml.jackson.core.type.TypeReference;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

public final class FileMinuteHistoryRepository implements MonitorPorts.MinuteHistoryRepository {
    private final Path file;
    private final ObjectMapper mapper;

    public FileMinuteHistoryRepository(Path file, ObjectMapper mapper) {
        this.file = file;
        this.mapper = mapper;
    }

    @Override
    public List<MinuteUsage> load() throws IOException {
        if (!Files.exists(file)) {
            return List.of();
        }
        return List.copyOf(mapper.readValue(Files.readAllBytes(file), new TypeReference<List<MinuteUsage>>() { }));
    }

    @Override
    public void save(List<MinuteUsage> points) throws IOException {
        AtomicFiles.replace(file, mapper.writeValueAsBytes(points));
    }
}
