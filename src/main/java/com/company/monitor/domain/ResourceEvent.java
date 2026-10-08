// src/main/java/com/company/monitor/domain/ResourceEvent.java
package com.company.monitor.domain;

import java.time.Instant;
import java.util.UUID;

public record ResourceEvent(int schemaVersion, String eventId, Instant occurredAt, Type type, String resource,
                            double usedPercent, double thresholdPercent, ResourceSnapshot snapshot) {
    public enum Type { THRESHOLD_REACHED, THRESHOLD_REMINDER, RECOVERED }

    public static ResourceEvent create(Instant at, Type type, String resource, double value,
                                       double threshold, ResourceSnapshot snapshot) {
        return new ResourceEvent(1, UUID.randomUUID().toString(), at, type, resource, value, threshold, snapshot);
    }

    public boolean warning() {
        return type != Type.RECOVERED;
    }
}
