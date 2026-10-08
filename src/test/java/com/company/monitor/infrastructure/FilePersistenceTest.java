// src/test/java/com/company/monitor/infrastructure/FilePersistenceTest.java
package com.company.monitor.infrastructure;

import com.company.monitor.domain.MonitorSettings;
import com.company.monitor.domain.ResourceEvent;
import com.company.monitor.domain.ResourceSnapshot;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import java.util.List;
import static org.junit.jupiter.api.Assertions.*;

class FilePersistenceTest {
    @TempDir Path directory;

    @Test
    void savesCompleteJsonWarningsAtomicallyAndRecoveryOnlyInJournal() throws Exception {
        var mapper = JsonSupport.create();
        Path warn = directory.resolve("warn");
        Path logs = directory.resolve("logs");
        var sink = new FileEventSink(warn, logs, mapper, 100000);
        var snapshot = new ResourceSnapshot(Instant.parse("2026-10-08T00:00:00Z"), "ol-host", 80.0,
                new ResourceSnapshot.MemoryUsage(100, 50, 50, 50),
                List.of(new ResourceSnapshot.DiskUsage("/", 100, 10, 90, 90)), List.of());
        var warning = ResourceEvent.create(snapshot.capturedAt(), ResourceEvent.Type.THRESHOLD_REACHED, "cpu", 80, 80, snapshot);
        sink.write(warning);
        sink.write(warning);
        sink.write(ResourceEvent.create(snapshot.capturedAt(), ResourceEvent.Type.RECOVERED, "cpu", 70, 80, snapshot));
        try (var files = Files.list(warn)) {
            var entries = files.toList();
            assertEquals(1, entries.size());
            assertTrue(entries.getFirst().getFileName().toString().endsWith(".json"));
            var json = mapper.readTree(Files.readString(entries.getFirst()));
            assertEquals(warning.eventId(), json.get("eventId").asText());
            assertEquals(80, json.at("/snapshot/cpuPercent").asDouble());
            assertEquals(50, json.at("/snapshot/memory/usedPercent").asDouble());
            assertEquals("/", json.at("/snapshot/disks/0/path").asText());
        }
        assertEquals(3, Files.readAllLines(logs.resolve("events.jsonl")).size());
    }

    @Test
    void persistsSettingsAndRejectsCorruptConfiguration() throws Exception {
        var repository = new FileSettingsRepository(directory.resolve("settings.json"), JsonSupport.create());
        var initial = repository.load();
        assertEquals(MonitorSettings.defaults(), initial);
        repository.save(initial);
        assertEquals(initial, repository.load());
        Files.writeString(directory.resolve("settings.json"), "{broken}");
        assertThrows(java.io.IOException.class, repository::load);
    }

    @Test
    void rotatesBoundedJournal() throws Exception {
        var sink = new FileEventSink(directory.resolve("warn"), directory.resolve("logs"), JsonSupport.create(), 1);
        var snapshot = new ResourceSnapshot(Instant.now(), "host", 10.0, null, List.of(), List.of());
        for (int index = 0; index < 10; index++) {
            sink.write(ResourceEvent.create(snapshot.capturedAt(), ResourceEvent.Type.RECOVERED, "cpu", 10, 80, snapshot));
        }
        try (var files = Files.list(directory.resolve("logs"))) {
            assertEquals(5, files.count());
        }
        try (var files = Files.list(directory.resolve("warn"))) {
            assertEquals(0, files.count());
        }
    }
}
