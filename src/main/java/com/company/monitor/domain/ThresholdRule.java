// src/main/java/com/company/monitor/domain/ThresholdRule.java
package com.company.monitor.domain;

public record ThresholdRule(boolean enabled, double thresholdPercent, double hysteresisPercent) {
    public ThresholdRule {
        if (!Double.isFinite(thresholdPercent) || thresholdPercent <= 0 || thresholdPercent > 100) {
            throw new IllegalArgumentException("임계치는 0보다 크고 100 이하여야 합니다.");
        }
        if (!Double.isFinite(hysteresisPercent) || hysteresisPercent < 0 || hysteresisPercent >= thresholdPercent) {
            throw new IllegalArgumentException("복구 여유폭은 0 이상이며 임계치보다 작아야 합니다.");
        }
    }
}
