// src/test/java/com/company/monitor/domain/MonitorSettingsTest.java
package com.company.monitor.domain;

import org.junit.jupiter.api.Test;
import java.util.List;
import static org.junit.jupiter.api.Assertions.*;

class MonitorSettingsTest {
    @Test
    void rejectsNonFiniteAndInvalidThresholds() {
        for (double value : new double[]{Double.NaN, Double.POSITIVE_INFINITY, -1, 0, 100.1}) {
            assertThrows(IllegalArgumentException.class, () -> new ThresholdRule(true, value, 0));
        }
        assertThrows(IllegalArgumentException.class, () -> new ThresholdRule(true, 80, 80));
        assertThrows(IllegalArgumentException.class, () -> new ThresholdRule(true, 80, Double.NaN));
        assertDoesNotThrow(() -> new ThresholdRule(true, 100, 0));
    }

    @Test
    void rejectsInvalidSamplingAndTraversalPaths() {
        var rule = new ThresholdRule(true, 80, 5);
        assertThrows(IllegalArgumentException.class, () -> new MonitorSettings(0, 1, 0, rule, rule, rule, List.of("/")));
        assertThrows(IllegalArgumentException.class, () -> new MonitorSettings(1, 0, 0, rule, rule, rule, List.of("/")));
        assertThrows(IllegalArgumentException.class, () -> new MonitorSettings(1, 1, -1, rule, rule, rule, List.of("/")));
        for (String path : List.of("relative", "/var/../etc", "/bad\\path", "/bad\u0000path")) {
            assertThrows(IllegalArgumentException.class, () -> new MonitorSettings(1, 1, 0, rule, rule, rule, List.of(path)));
        }
        assertThrows(IllegalArgumentException.class, () -> new MonitorSettings(1, 1, 0, rule, rule, rule, List.of("/", "/")));
    }
}
