// src/test/java/com/company/monitor/domain/ThresholdEngineTest.java
package com.company.monitor.domain;

import org.junit.jupiter.api.Test;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import static org.junit.jupiter.api.Assertions.*;

class ThresholdEngineTest {
    private final ThresholdEngine engine = new ThresholdEngine();
    private final Instant start = Instant.parse("2026-10-08T00:00:00Z");

    private ResourceSnapshot snapshot(double cpu, Instant time) {
        return new ResourceSnapshot(time, "ol9-host", cpu,
                new ResourceSnapshot.MemoryUsage(1000, 500, 500, 50), List.of(), List.of());
    }

    private MonitorSettings settings(int consecutive, int repeat) {
        return new MonitorSettings(5, consecutive, repeat, new ThresholdRule(true, 80, 5),
                new ThresholdRule(false, 85, 5), new ThresholdRule(false, 90, 5), List.of("/"));
    }

    @Test
    void equalityReachesThresholdAndKeepsCompleteSnapshot() {
        var snapshot = snapshot(80, start);
        var result = engine.evaluate(Map.of(), snapshot, settings(1, 300));
        assertEquals(1, result.events().size());
        assertEquals(ResourceEvent.Type.THRESHOLD_REACHED, result.events().getFirst().type());
        assertSame(snapshot, result.events().getFirst().snapshot());
        assertTrue(result.states().get("cpu").active());
    }

    @Test
    void requiresConsecutiveSamplesAndResetsBelowThreshold() {
        var first = engine.evaluate(Map.of(), snapshot(90, start), settings(2, 300));
        assertTrue(first.events().isEmpty());
        var below = engine.evaluate(first.states(), snapshot(79, start.plusSeconds(5)), settings(2, 300));
        var again = engine.evaluate(below.states(), snapshot(90, start.plusSeconds(10)), settings(2, 300));
        assertTrue(again.events().isEmpty());
        var reached = engine.evaluate(again.states(), snapshot(80, start.plusSeconds(15)), settings(2, 300));
        assertEquals(1, reached.events().size());
    }

    @Test
    void recoversOnlyBelowHysteresisBoundary() {
        var reached = engine.evaluate(Map.of(), snapshot(90, start), settings(1, 300));
        var boundary = engine.evaluate(reached.states(), snapshot(75, start.plusSeconds(5)), settings(1, 300));
        assertTrue(boundary.events().isEmpty());
        assertTrue(boundary.states().get("cpu").active());
        var recovered = engine.evaluate(boundary.states(), snapshot(74.9, start.plusSeconds(10)), settings(1, 300));
        assertEquals(ResourceEvent.Type.RECOVERED, recovered.events().getFirst().type());
        assertFalse(recovered.states().get("cpu").active());
    }

    @Test
    void repeatsOnlyAfterConfiguredInterval() {
        var first = engine.evaluate(Map.of(), snapshot(90, start), settings(1, 60));
        var early = engine.evaluate(first.states(), snapshot(90, start.plusSeconds(59)), settings(1, 60));
        assertTrue(early.events().isEmpty());
        var repeat = engine.evaluate(early.states(), snapshot(90, start.plusSeconds(60)), settings(1, 60));
        assertEquals(ResourceEvent.Type.THRESHOLD_REMINDER, repeat.events().getFirst().type());
        var disabled = engine.evaluate(repeat.states(), snapshot(90, start.plusSeconds(1000)), settings(1, 0));
        assertTrue(disabled.events().isEmpty());
    }

    @Test
    void missingMetricsNeverProduceFalseRecoveryOrContinuousConfirmation() {
        var reached = engine.evaluate(Map.of(), snapshot(90, start), settings(1, 300));
        var missing = new ResourceSnapshot(start.plusSeconds(5), "ol9-host", null, null, List.of(), List.of("CPU read failed"));
        var result = engine.evaluate(reached.states(), missing, settings(1, 300));
        assertTrue(result.events().isEmpty());
        assertTrue(result.states().get("cpu").active());
        var first = engine.evaluate(Map.of(), snapshot(90, start), settings(2, 300));
        var gap = engine.evaluate(first.states(), missing, settings(2, 300));
        var after = engine.evaluate(gap.states(), snapshot(90, start.plusSeconds(10)), settings(2, 300));
        assertTrue(after.events().isEmpty());
    }

    @Test
    void monitorsEachConfiguredDiskAndDropsRemovedPaths() {
        var settings = new MonitorSettings(5, 1, 0, new ThresholdRule(false, 80, 5),
                new ThresholdRule(false, 85, 5), new ThresholdRule(true, 90, 5), List.of("/", "/var"));
        var snapshot = new ResourceSnapshot(start, "ol9-host", 10.0, null,
                List.of(new ResourceSnapshot.DiskUsage("/", 100, 10, 90, 90),
                        new ResourceSnapshot.DiskUsage("/var", 100, 20, 80, 80)), List.of());
        var result = engine.evaluate(Map.of("disk:/removed", new ThresholdEngine.State(0, true, start)), snapshot, settings);
        assertEquals("disk:/", result.events().getFirst().resource());
        assertFalse(result.states().containsKey("disk:/removed"));
    }

    @Test
    void disablingRuleClearsAlarmWithoutGeneratingWarning() {
        var first = engine.evaluate(Map.of(), snapshot(90, start), settings(1, 0));
        var disabled = new MonitorSettings(5, 1, 0, new ThresholdRule(false, 80, 5),
                new ThresholdRule(false, 85, 5), new ThresholdRule(false, 90, 5), List.of("/"));
        var result = engine.evaluate(first.states(), snapshot(99, start.plusSeconds(5)), disabled);
        assertTrue(result.events().isEmpty());
        assertFalse(result.states().get("cpu").active());
    }
}
