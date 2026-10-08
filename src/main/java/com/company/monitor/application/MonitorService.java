// src/main/java/com/company/monitor/application/MonitorService.java
package com.company.monitor.application;

import com.company.monitor.domain.MonitorSettings;
import com.company.monitor.domain.ResourceEvent;
import com.company.monitor.domain.ResourceSnapshot;
import com.company.monitor.domain.ThresholdEngine;
import java.io.IOException;
import java.time.Clock;
import java.time.Instant;
import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class MonitorService {
    public record Status(MonitorSettings settings, ResourceSnapshot snapshot, Map<String, ThresholdEngine.State> states,
                         List<ResourceEvent> recentEvents, String lastError, long settingsVersion, Instant lastCycleAt) { }

    private final MonitorPorts.Collector collector;
    private final MonitorPorts.SettingsRepository repository;
    private final MonitorPorts.EventSink sink;
    private final ThresholdEngine engine;
    private final Clock clock;
    private final ArrayDeque<ResourceEvent> recent = new ArrayDeque<>();
    private MonitorSettings settings;
    private ResourceSnapshot snapshot;
    private Map<String, ThresholdEngine.State> states = Map.of();
    private List<ResourceEvent> pending = List.of();
    private Map<String, ThresholdEngine.State> pendingStates;
    private long settingsVersion = 1;
    private long nextSampleNanos;
    private String lastError;
    private Instant lastCycleAt;
    private volatile Status publishedStatus;

    public MonitorService(MonitorPorts.Collector collector, MonitorPorts.SettingsRepository repository,
                          MonitorPorts.EventSink sink, ThresholdEngine engine, Clock clock) throws IOException {
        this.collector = collector;
        this.repository = repository;
        this.sink = sink;
        this.engine = engine;
        this.clock = clock;
        this.settings = repository.load();
        publishStatus();
    }

    public synchronized void updateSettings(MonitorSettings value, long expectedVersion) throws IOException {
        if (expectedVersion != settingsVersion) {
            throw new IllegalStateException("설정이 다른 화면에서 변경되었습니다. 새로 불러온 뒤 저장하세요.");
        }
        repository.save(value);
        settings = value;
        states = resetCounts(states);
        if (pendingStates != null) {
            pendingStates = resetCounts(pendingStates);
        }
        settingsVersion++;
        nextSampleNanos = 0;
        publishStatus();
    }

    public synchronized void tick(long nowNanos) {
        if (nextSampleNanos != 0 && nowNanos - nextSampleNanos < 0) {
            return;
        }
        nextSampleNanos = nowNanos + settings.sampleIntervalSeconds() * 1_000_000_000L;
        try {
            flushPending();
            snapshot = collector.collect(settings, clock.instant());
            if (snapshot.cpuPercent() == null && snapshot.errors().isEmpty()) {
                nextSampleNanos = nowNanos + 1_000_000_000L;
            }
            ThresholdEngine.Evaluation evaluation = engine.evaluate(states, snapshot, settings);
            pending = evaluation.events();
            pendingStates = evaluation.states();
            flushPending();
            lastError = null;
            lastCycleAt = clock.instant();
        } catch (Exception exception) {
            lastError = exception.getClass().getSimpleName() + ": " + exception.getMessage();
            System.err.println("Monitoring cycle failed: " + lastError);
        } finally {
            publishStatus();
        }
    }

    private void flushPending() throws IOException {
        while (!pending.isEmpty()) {
            ResourceEvent event = pending.getFirst();
            sink.write(event);
            recent.addFirst(event);
            while (recent.size() > 100) {
                recent.removeLast();
            }
            pending = List.copyOf(pending.subList(1, pending.size()));
        }
        if (pendingStates != null) {
            states = pendingStates;
            pendingStates = null;
        }
    }

    private static Map<String, ThresholdEngine.State> resetCounts(Map<String, ThresholdEngine.State> value) {
        Map<String, ThresholdEngine.State> result = new HashMap<>();
        value.forEach((key, state) -> result.put(key, new ThresholdEngine.State(0, state.active(), state.lastWarning())));
        return Map.copyOf(result);
    }

    private void publishStatus() {
        publishedStatus = new Status(settings, snapshot, Map.copyOf(states), List.copyOf(recent), lastError, settingsVersion, lastCycleAt);
    }

    public Status status() {
        return publishedStatus;
    }
}
