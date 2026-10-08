// src/main/java/com/company/monitor/domain/ThresholdEngine.java
package com.company.monitor.domain;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class ThresholdEngine {
    public record State(int consecutive, boolean active, Instant lastWarning) { }
    public record Evaluation(Map<String, State> states, List<ResourceEvent> events) {
        public Evaluation {
            states = Map.copyOf(states);
            events = List.copyOf(events);
        }
    }

    public Evaluation evaluate(Map<String, State> previous, ResourceSnapshot snapshot, MonitorSettings settings) {
        Map<String, State> next = new HashMap<>(previous);
        List<ResourceEvent> events = new ArrayList<>();
        Map<String, ThresholdRule> rules = new LinkedHashMap<>();
        rules.put("cpu", settings.cpu());
        rules.put("memory", settings.memory());
        settings.diskPaths().forEach(path -> rules.put("disk:" + path, settings.disk()));
        next.keySet().retainAll(rules.keySet());
        Map<String, Double> values = new HashMap<>();
        if (snapshot.cpuPercent() != null) {
            values.put("cpu", snapshot.cpuPercent());
        }
        if (snapshot.memory() != null) {
            values.put("memory", snapshot.memory().usedPercent());
        }
        snapshot.disks().forEach(disk -> values.put("disk:" + disk.path(), disk.usedPercent()));
        rules.forEach((resource, rule) -> {
            State state = next.getOrDefault(resource, new State(0, false, null));
            Double value = values.get(resource);
            if (!rule.enabled()) {
                next.put(resource, new State(0, false, null));
            } else if (value == null) {
                next.put(resource, new State(0, state.active(), state.lastWarning()));
            } else if (!state.active()) {
                int count = value >= rule.thresholdPercent() ? state.consecutive() + 1 : 0;
                if (count >= settings.consecutiveSamples()) {
                    events.add(ResourceEvent.create(snapshot.capturedAt(), ResourceEvent.Type.THRESHOLD_REACHED,
                            resource, value, rule.thresholdPercent(), snapshot));
                    next.put(resource, new State(0, true, snapshot.capturedAt()));
                } else {
                    next.put(resource, new State(count, false, null));
                }
            } else if (value < rule.thresholdPercent() - rule.hysteresisPercent()) {
                events.add(ResourceEvent.create(snapshot.capturedAt(), ResourceEvent.Type.RECOVERED,
                        resource, value, rule.thresholdPercent(), snapshot));
                next.put(resource, new State(0, false, null));
            } else if (value >= rule.thresholdPercent() && settings.repeatIntervalSeconds() > 0
                    && Duration.between(state.lastWarning(), snapshot.capturedAt()).getSeconds() >= settings.repeatIntervalSeconds()) {
                events.add(ResourceEvent.create(snapshot.capturedAt(), ResourceEvent.Type.THRESHOLD_REMINDER,
                        resource, value, rule.thresholdPercent(), snapshot));
                next.put(resource, new State(0, true, snapshot.capturedAt()));
            }
        });
        return new Evaluation(next, events);
    }
}
