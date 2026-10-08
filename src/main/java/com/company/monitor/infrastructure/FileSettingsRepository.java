// src/main/java/com/company/monitor/infrastructure/FileSettingsRepository.java
package com.company.monitor.infrastructure;

import com.company.monitor.application.MonitorPorts;
import com.company.monitor.domain.MonitorSettings;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

public final class FileSettingsRepository implements MonitorPorts.SettingsRepository {
    private final Path file;
    private final ObjectMapper mapper;

    public FileSettingsRepository(Path file, ObjectMapper mapper) {
        this.file = file;
        this.mapper = mapper;
    }

    @Override
    public MonitorSettings load() throws IOException {
        if (!Files.exists(file)) {
            MonitorSettings settings = MonitorSettings.defaults();
            save(settings);
            return settings;
        }
        return mapper.readValue(Files.readAllBytes(file), MonitorSettings.class);
    }

    @Override
    public void save(MonitorSettings settings) throws IOException {
        AtomicFiles.replace(file, mapper.writerWithDefaultPrettyPrinter().writeValueAsBytes(settings));
    }
}
