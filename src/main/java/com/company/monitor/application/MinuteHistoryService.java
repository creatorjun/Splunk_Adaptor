// src/main/java/com/company/monitor/application/MinuteHistoryService.java
package com.company.monitor.application;

import com.company.monitor.domain.MinuteUsage;
import com.company.monitor.domain.MonitorSettings;
import com.company.monitor.domain.ResourceSnapshot;
import java.io.IOException;
import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

public final class MinuteHistoryService {
    public static final int INTERVAL_SECONDS = 60;
    public static final int RETENTION_MINUTES = 60;
    public record History(int intervalSeconds, int retentionMinutes, List<MinuteUsage> points, String lastError) { }

    private final MonitorPorts.Collector collector;
    private final MonitorPorts.MinuteHistoryRepository repository;
    private final Clock clock;
    private volatile History published;
    private long nextSampleNanos;

    public MinuteHistoryService(MonitorPorts.Collector collector, MonitorPorts.MinuteHistoryRepository repository,
                                Clock clock) throws IOException {
        this.collector = collector;
        this.repository = repository;
        this.clock = clock;
        List<MinuteUsage> points = repository.load().stream()
                .filter(point -> point.capturedAt().isAfter(clock.instant().minus(Duration.ofMinutes(RETENTION_MINUTES))))
                .sorted(Comparator.comparing(MinuteUsage::capturedAt)).toList();
        if (points.size() > RETENTION_MINUTES) {
            points = points.subList(points.size() - RETENTION_MINUTES, points.size());
        }
        published = new History(INTERVAL_SECONDS, RETENTION_MINUTES, List.copyOf(points), null);
    }

    public synchronized void tick(long nowNanos, MonitorSettings settings) {
        if (nextSampleNanos != 0 && nowNanos - nextSampleNanos < 0) {
            return;
        }
        nextSampleNanos = nowNanos + INTERVAL_SECONDS * 1_000_000_000L;
        try {
            ResourceSnapshot snapshot = collector.collect(settings, clock.instant());
            if (snapshot.cpuPercent() == null && snapshot.errors().isEmpty()) {
                nextSampleNanos = nowNanos + 1_000_000_000L;
                return;
            }
            List<MinuteUsage> points = new ArrayList<>(published.points());
            Instant cutoff = snapshot.capturedAt().minus(Duration.ofMinutes(RETENTION_MINUTES));
            points.removeIf(point -> !point.capturedAt().isAfter(cutoff));
            if (!points.isEmpty()) {
                long elapsedMillis = Duration.between(points.getLast().capturedAt(), snapshot.capturedAt()).toMillis();
                if (elapsedMillis < INTERVAL_SECONDS * 1000L) {
                    nextSampleNanos = nowNanos + Math.min(INTERVAL_SECONDS * 1000L,
                            INTERVAL_SECONDS * 1000L - elapsedMillis) * 1_000_000L;
                    return;
                }
            }
            if (points.isEmpty() || snapshot.capturedAt().isAfter(points.getLast().capturedAt())) {
                points.add(MinuteUsage.from(snapshot));
            }
            while (points.size() > RETENTION_MINUTES) {
                points.removeFirst();
            }
            repository.save(List.copyOf(points));
            published = new History(INTERVAL_SECONDS, RETENTION_MINUTES, List.copyOf(points), null);
        } catch (Exception exception) {
            published = new History(INTERVAL_SECONDS, RETENTION_MINUTES, published.points(),
                    exception.getClass().getSimpleName() + ": " + exception.getMessage());
            System.err.println("Minute history failed: " + published.lastError());
        }
    }

    public History history() {
        History current = published;
        Instant cutoff = clock.instant().minus(Duration.ofMinutes(RETENTION_MINUTES));
        return new History(current.intervalSeconds(), current.retentionMinutes(),
                current.points().stream().filter(point -> point.capturedAt().isAfter(cutoff)).toList(), current.lastError());
    }
}
