// src/test/java/com/company/monitor/application/MinuteHistoryServiceTest.java
package com.company.monitor.application;

import com.company.monitor.domain.MinuteUsage;
import com.company.monitor.domain.MonitorSettings;
import com.company.monitor.domain.ResourceSnapshot;
import org.junit.jupiter.api.Test;
import java.io.IOException;
import java.time.Clock;
import java.time.Instant;
import java.time.ZoneId;
import java.time.ZoneOffset;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import static org.junit.jupiter.api.Assertions.*;

class MinuteHistoryServiceTest {
    private static final Instant START = Instant.parse("2026-10-08T03:00:00Z");

    private static final class MutableClock extends Clock {
        private Instant now = START;
        @Override public ZoneId getZone() { return ZoneOffset.UTC; }
        @Override public Clock withZone(ZoneId zone) { return this; }
        @Override public Instant instant() { return now; }
    }

    private static final class Repository implements MonitorPorts.MinuteHistoryRepository {
        private List<MinuteUsage> points = List.of();
        private boolean fail;
        @Override public List<MinuteUsage> load() { return points; }
        @Override public void save(List<MinuteUsage> value) throws IOException {
            if (fail) throw new IOException("disk full");
            points = List.copyOf(value);
        }
    }

    private ResourceSnapshot snapshot(Instant at, Double cpu, List<String> errors) {
        return new ResourceSnapshot(at, "host", cpu, new ResourceSnapshot.MemoryUsage(100, 25, 75, 75),
                List.of(new ResourceSnapshot.DiskUsage("/", 100, 20, 80, 80),
                        new ResourceSnapshot.DiskUsage("/var", 100, 10, 90, 90)), errors);
    }

    @Test
    void warmsCpuThenCollectsOncePerSixtySecondsRegardlessOfAlarmInterval() throws Exception {
        var clock = new MutableClock();
        var repository = new Repository();
        var calls = new AtomicInteger();
        var service = new MinuteHistoryService((settings, at) -> snapshot(at, calls.getAndIncrement() == 0 ? null : 40.0, List.of()), repository, clock);
        var original = MonitorSettings.defaults();
        var slow = new MonitorSettings(300, 1, 0, original.cpu(), original.memory(), original.disk(), original.diskPaths());
        service.tick(1, slow);
        assertTrue(service.history().points().isEmpty());
        clock.now = START.plusSeconds(1);
        service.tick(1_000_000_001L, slow);
        assertEquals(1, service.history().points().size());
        clock.now = START.plusSeconds(60);
        service.tick(60_000_000_001L, slow);
        assertEquals(2, calls.get());
        clock.now = START.plusSeconds(61);
        service.tick(61_000_000_001L, slow);
        assertEquals(3, calls.get());
        assertEquals(2, service.history().points().size());
        assertEquals(90, service.history().points().getFirst().diskPercent());
        assertEquals(60, service.history().intervalSeconds());
    }

    @Test
    void capsHistoryAndPrunesOlderThanSixtyMinutes() throws Exception {
        var clock = new MutableClock();
        var repository = new Repository();
        var service = new MinuteHistoryService((settings, at) -> snapshot(at, 40.0, List.of()), repository, clock);
        for (int minute = 0; minute < 80; minute++) {
            clock.now = START.plusSeconds(minute * 60L);
            service.tick(1 + minute * 60_000_000_000L, MonitorSettings.defaults());
        }
        assertEquals(60, service.history().points().size());
        assertEquals(START.plusSeconds(20 * 60L), service.history().points().getFirst().capturedAt());
        clock.now = START.plusSeconds(180 * 60L);
        assertTrue(service.history().points().isEmpty());
    }

    @Test
    void preservesPublishedHistoryWhenStorageFailsThenRecovers() throws Exception {
        var clock = new MutableClock();
        var repository = new Repository();
        var service = new MinuteHistoryService((settings, at) -> snapshot(at, 40.0, List.of()), repository, clock);
        service.tick(1, MonitorSettings.defaults());
        repository.fail = true;
        clock.now = START.plusSeconds(60);
        service.tick(60_000_000_001L, MonitorSettings.defaults());
        assertEquals(1, service.history().points().size());
        assertNotNull(service.history().lastError());
        repository.fail = false;
        clock.now = START.plusSeconds(120);
        service.tick(120_000_000_001L, MonitorSettings.defaults());
        assertEquals(2, service.history().points().size());
        assertNull(service.history().lastError());
    }

    @Test
    void recordsMissingMetricsAsNullInsteadOfZero() throws Exception {
        var service = new MinuteHistoryService((settings, at) -> new ResourceSnapshot(at, "host", null,
                null, List.of(), List.of("read failed")), new Repository(), new MutableClock());
        service.tick(1, MonitorSettings.defaults());
        var point = service.history().points().getFirst();
        assertNull(point.cpuPercent());
        assertNull(point.memoryPercent());
        assertNull(point.diskPercent());
        assertEquals(List.of("read failed"), point.errors());
    }

    @Test
    void restartWaitsForNextMinuteInsteadOfDuplicatingRecentPoint() throws Exception {
        var clock = new MutableClock();
        var repository = new Repository();
        repository.points = List.of(MinuteUsage.from(snapshot(START.minusSeconds(20), 30.0, List.of())));
        var service = new MinuteHistoryService((settings, at) -> snapshot(at, 40.0, List.of()), repository, clock);
        service.tick(1, MonitorSettings.defaults());
        assertEquals(1, service.history().points().size());
        clock.now = START.plusSeconds(39);
        service.tick(39_000_000_001L, MonitorSettings.defaults());
        assertEquals(1, service.history().points().size());
        clock.now = START.plusSeconds(40);
        service.tick(40_000_000_001L, MonitorSettings.defaults());
        assertEquals(2, service.history().points().size());
    }

    @Test
    void restoresRecentHistoryAndDropsExpiredPoints() throws Exception {
        var clock = new MutableClock();
        var repository = new Repository();
        repository.points = List.of(MinuteUsage.from(snapshot(START.minusSeconds(3601), 30.0, List.of())),
                MinuteUsage.from(snapshot(START.minusSeconds(120), 40.0, List.of())));
        var service = new MinuteHistoryService((settings, at) -> snapshot(at, 40.0, List.of()), repository, clock);
        assertEquals(1, service.history().points().size());
        assertEquals(40, service.history().points().getFirst().cpuPercent());
    }
}
