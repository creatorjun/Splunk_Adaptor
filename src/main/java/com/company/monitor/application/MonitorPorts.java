// src/main/java/com/company/monitor/application/MonitorPorts.java
package com.company.monitor.application;

import com.company.monitor.domain.MonitorSettings;
import com.company.monitor.domain.ResourceEvent;
import com.company.monitor.domain.ResourceSnapshot;
import com.company.monitor.domain.MinuteUsage;
import java.io.IOException;
import java.time.Instant;
import java.util.List;

public final class MonitorPorts {
    private MonitorPorts() { }

    public interface Collector {
        ResourceSnapshot collect(MonitorSettings settings, Instant at);
    }

    public interface SettingsRepository {
        MonitorSettings load() throws IOException;
        void save(MonitorSettings settings) throws IOException;
    }

    public interface EventSink {
        void write(ResourceEvent event) throws IOException;
    }

    public interface MinuteHistoryRepository {
        List<MinuteUsage> load() throws IOException;
        void save(List<MinuteUsage> points) throws IOException;
    }
}
