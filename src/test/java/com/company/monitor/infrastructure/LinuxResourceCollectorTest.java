// src/test/java/com/company/monitor/infrastructure/LinuxResourceCollectorTest.java
package com.company.monitor.infrastructure;

import com.company.monitor.domain.MonitorSettings;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import static org.junit.jupiter.api.Assertions.*;

class LinuxResourceCollectorTest {
    @TempDir Path directory;

    @Test
    void usesHostCountersExcludesGuestAndCalculatesAvailableMemory() throws Exception {
        Files.writeString(directory.resolve("stat"), "cpu 100 0 50 850 0 0 0 0 500 500\n");
        Files.writeString(directory.resolve("meminfo"), "MemTotal: 1000 kB\nMemAvailable: 250 kB\nMemFree: 10 kB\n");
        var collector = new LinuxResourceCollector(directory, directory, "oracle-host");
        var first = collector.collect(MonitorSettings.defaults(), Instant.now());
        assertNull(first.cpuPercent());
        assertEquals(75, first.memory().usedPercent());
        assertEquals(1024000, first.memory().totalBytes());
        assertEquals("oracle-host", first.host());
        assertEquals(1, first.disks().size());
        Files.writeString(directory.resolve("stat"), "cpu 170 0 60 870 0 0 0 0 9000 9000\n");
        var second = collector.collect(MonitorSettings.defaults(), Instant.now());
        assertEquals(80, second.cpuPercent(), .0001);
        assertTrue(second.errors().isEmpty());
        assertTrue(second.disks().getFirst().totalBytes() > 0);
    }

    @Test
    void reportsMissingAndResetCountersWithoutFabricatingZero() throws Exception {
        Files.writeString(directory.resolve("stat"), "cpu 100 0 50 850 0 0 0 0\n");
        var collector = new LinuxResourceCollector(directory, directory, "oracle-host");
        var missing = collector.collect(MonitorSettings.defaults(), Instant.now());
        assertNull(missing.memory());
        assertFalse(missing.errors().isEmpty());
        Files.writeString(directory.resolve("stat"), "cpu 10 0 5 85 0 0 0 0\n");
        var reset = collector.collect(MonitorSettings.defaults(), Instant.now());
        assertNull(reset.cpuPercent());
        assertTrue(reset.errors().stream().anyMatch(value -> value.startsWith("CPU:")));
    }
}
