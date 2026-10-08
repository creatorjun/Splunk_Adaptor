// src/test/java/com/company/monitor/infrastructure/MonitorHttpServerTest.java
package com.company.monitor.infrastructure;

import com.company.monitor.application.MonitorService;
import com.company.monitor.application.MinuteHistoryService;
import com.company.monitor.domain.ResourceSnapshot;
import com.company.monitor.domain.ThresholdEngine;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import java.net.InetSocketAddress;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.file.Path;
import java.time.Clock;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executors;
import static org.junit.jupiter.api.Assertions.*;

class MonitorHttpServerTest {
    @TempDir Path directory;
    private static final String TOKEN = "test-token-with-at-least-24-characters";

    @Test
    void protectsApiValidatesUpdatesAndServesDashboard() throws Exception {
        var mapper = JsonSupport.create();
        var repository = new FileSettingsRepository(directory.resolve("settings.json"), mapper);
        var service = new MonitorService((settings, at) -> new ResourceSnapshot(at, "host", 20.0,
                new ResourceSnapshot.MemoryUsage(100, 80, 20, 20), List.of(), List.of()),
                repository, event -> { }, new ThresholdEngine(), Clock.systemUTC());
        var history = new MinuteHistoryService((settings, at) -> new ResourceSnapshot(at, "host", 30.0,
                new ResourceSnapshot.MemoryUsage(100, 80, 20, 20), List.of(), List.of()),
                new FileMinuteHistoryRepository(directory.resolve("history.json"), mapper), Clock.systemUTC());
        history.tick(1, service.status().settings());
        try (var executor = Executors.newVirtualThreadPerTaskExecutor();
             var web = new MonitorHttpServer(new InetSocketAddress("127.0.0.1", 0), service, history, mapper, TOKEN, executor);
             var client = HttpClient.newHttpClient()) {
            web.start();
            String base = "http://127.0.0.1:" + web.port();
            var unauthorized = client.send(HttpRequest.newBuilder(URI.create(base + "/api/status")).GET().build(), HttpResponse.BodyHandlers.ofString());
            assertEquals(401, unauthorized.statusCode());
            var unauthorizedHistory = client.send(HttpRequest.newBuilder(URI.create(base + "/api/history")).GET().build(), HttpResponse.BodyHandlers.ofString());
            assertEquals(401, unauthorizedHistory.statusCode());
            var page = client.send(HttpRequest.newBuilder(URI.create(base + "/")).GET().build(), HttpResponse.BodyHandlers.ofString());
            assertEquals(200, page.statusCode());
            assertTrue(page.body().contains("시스템 리소스 현황"));
            assertTrue(page.body().contains("id=\"usage-chart\""));
            var historyResponse = client.send(HttpRequest.newBuilder(URI.create(base + "/api/history"))
                    .header("Authorization", "Bearer " + TOKEN).GET().build(), HttpResponse.BodyHandlers.ofString());
            assertEquals(200, historyResponse.statusCode());
            var historyJson = mapper.readTree(historyResponse.body());
            assertEquals(60, historyJson.get("intervalSeconds").asInt());
            assertEquals(1, historyJson.get("points").size());
            assertEquals(30, historyJson.at("/points/0/cpuPercent").asDouble());
            service.tick(1);
            var status = client.send(HttpRequest.newBuilder(URI.create(base + "/api/status"))
                    .header("Authorization", "Bearer " + TOKEN).GET().build(), HttpResponse.BodyHandlers.ofString());
            assertEquals(200, status.statusCode());
            var update = mapper.writeValueAsString(Map.of("expectedVersion", 1, "settings", service.status().settings()));
            var request = HttpRequest.newBuilder(URI.create(base + "/api/settings"))
                    .header("Authorization", "Bearer " + TOKEN).header("Content-Type", "application/json")
                    .PUT(HttpRequest.BodyPublishers.ofString(update)).build();
            assertEquals(200, client.send(request, HttpResponse.BodyHandlers.ofString()).statusCode());
            assertEquals(409, client.send(request, HttpResponse.BodyHandlers.ofString()).statusCode());
            var invalid = HttpRequest.newBuilder(URI.create(base + "/api/settings"))
                    .header("Authorization", "Bearer " + TOKEN).header("Content-Type", "application/json")
                    .PUT(HttpRequest.BodyPublishers.ofString("{\"expectedVersion\":2,\"settings\":{}}")).build();
            assertEquals(400, client.send(invalid, HttpResponse.BodyHandlers.ofString()).statusCode());
            var large = HttpRequest.newBuilder(URI.create(base + "/api/settings"))
                    .header("Authorization", "Bearer " + TOKEN).header("Content-Type", "application/json")
                    .PUT(HttpRequest.BodyPublishers.ofString(" ".repeat(17000))).build();
            assertEquals(413, client.send(large, HttpResponse.BodyHandlers.ofString()).statusCode());
        }
    }
}
