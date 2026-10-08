// src/test/java/com/company/monitor/infrastructure/FileMinuteHistoryRepositoryTest.java
package com.company.monitor.infrastructure;

import com.company.monitor.domain.MinuteUsage;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import java.util.List;
import static org.junit.jupiter.api.Assertions.*;

class FileMinuteHistoryRepositoryTest {
    @TempDir Path directory;

    @Test
    void persistsMinuteRecordsIncludingMissingValuesAcrossInstances() throws Exception {
        var file = directory.resolve("history.json");
        var repository = new FileMinuteHistoryRepository(file, JsonSupport.create());
        assertTrue(repository.load().isEmpty());
        var points = List.of(new MinuteUsage(Instant.parse("2026-10-08T03:00:00Z"), 40.0, 75.0, 80.0, List.of()),
                new MinuteUsage(Instant.parse("2026-10-08T03:01:00Z"), null, 75.0, null, List.of("CPU missing")));
        repository.save(points);
        assertEquals(points, new FileMinuteHistoryRepository(file, JsonSupport.create()).load());
        try (var files = Files.list(directory)) {
            assertEquals(1, files.count());
        }
    }

    @Test
    void rejectsMalformedHistoryInsteadOfInventingRecords() throws Exception {
        var file = directory.resolve("history.json");
        Files.writeString(file, "[{\"capturedAt\":\"bad\"}]");
        assertThrows(java.io.IOException.class, () -> new FileMinuteHistoryRepository(file, JsonSupport.create()).load());
    }
}
