// src/test/java/com/company/monitor/application/MonitorServiceTest.java
package com.company.monitor.application;

import com.company.monitor.domain.MonitorSettings;
import com.company.monitor.domain.ResourceEvent;
import com.company.monitor.domain.ResourceSnapshot;
import com.company.monitor.domain.ThresholdEngine;
import com.company.monitor.domain.ThresholdRule;
import org.junit.jupiter.api.Test;
import java.io.IOException;
import java.time.Clock;
import java.time.Instant;
import java.time.ZoneOffset;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import static org.junit.jupiter.api.Assertions.*;

class MonitorServiceTest {
    private static final Clock CLOCK = Clock.fixed(Instant.parse("2026-10-08T00:00:00Z"), ZoneOffset.UTC);

    private static class Repository implements MonitorPorts.SettingsRepository {
        MonitorSettings settings = MonitorSettings.defaults();
        boolean fail;
        @Override public MonitorSettings load() { return settings; }
        @Override public void save(MonitorSettings value) throws IOException {
            if (fail) throw new IOException("disk full");
            settings = value;
        }
    }

    private MonitorPorts.Collector collector() {
        return (settings, at) -> new ResourceSnapshot(at, "host", 50.0,
                new ResourceSnapshot.MemoryUsage(100, 60, 40, 40), List.of(), List.of());
    }

    private MonitorSettings lowThreshold() {
        return new MonitorSettings(300, 1, 0, new ThresholdRule(true, 40, 5),
                new ThresholdRule(false, 85, 5), new ThresholdRule(false, 90, 5), List.of("/"));
    }

    @Test
    void appliesUpdateOnNextTickWithoutWaitingForOldInterval() throws Exception {
        var repository = new Repository();
        List<ResourceEvent> events = new ArrayList<>();
        var service = new MonitorService(collector(), repository, events::add, new ThresholdEngine(), CLOCK);
        service.tick(1);
        assertTrue(events.isEmpty());
        service.updateSettings(lowThreshold(), 1);
        service.tick(2);
        assertEquals(1, events.size());
        assertEquals(40, events.getFirst().thresholdPercent());
        assertEquals(2, service.status().settingsVersion());
    }

    @Test
    void rejectsFailedPersistenceAndConcurrentStaleUpdates() throws Exception {
        var repository = new Repository();
        var service = new MonitorService(collector(), repository, event -> { }, new ThresholdEngine(), CLOCK);
        repository.fail = true;
        assertThrows(IOException.class, () -> service.updateSettings(lowThreshold(), 1));
        assertEquals(MonitorSettings.defaults(), service.status().settings());
        assertEquals(1, service.status().settingsVersion());
        repository.fail = false;
        service.updateSettings(lowThreshold(), 1);
        assertThrows(IllegalStateException.class, () -> service.updateSettings(lowThreshold(), 1));
    }

    @Test
    void retriesFailedWarningWithSameIdAndCommitsOnlyAfterPersistence() throws Exception {
        var repository = new Repository();
        repository.settings = lowThreshold();
        List<String> attempts = new ArrayList<>();
        AtomicBoolean fail = new AtomicBoolean(true);
        var service = new MonitorService(collector(), repository, event -> {
            attempts.add(event.eventId());
            if (fail.get()) throw new IOException("warn disk full");
        }, new ThresholdEngine(), CLOCK);
        service.tick(1);
        assertNotNull(service.status().lastError());
        assertTrue(service.status().recentEvents().isEmpty());
        assertFalse(service.status().states().containsKey("cpu"));
        fail.set(false);
        service.tick(300_000_000_002L);
        assertEquals(2, attempts.size());
        assertEquals(attempts.getFirst(), attempts.getLast());
        assertEquals(1, service.status().recentEvents().size());
        assertTrue(service.status().states().get("cpu").active());
        assertNull(service.status().lastError());
    }
}
