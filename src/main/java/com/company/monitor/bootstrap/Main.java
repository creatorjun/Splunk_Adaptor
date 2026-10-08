// src/main/java/com/company/monitor/bootstrap/Main.java
package com.company.monitor.bootstrap;

import com.company.monitor.application.MonitorService;
import com.company.monitor.domain.ThresholdEngine;
import com.company.monitor.infrastructure.FileEventSink;
import com.company.monitor.infrastructure.FileSettingsRepository;
import com.company.monitor.infrastructure.JsonSupport;
import com.company.monitor.infrastructure.LinuxResourceCollector;
import com.company.monitor.infrastructure.MonitorHttpServer;
import java.net.InetSocketAddress;
import java.nio.file.Path;
import java.time.Clock;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

public final class Main {
    private Main() { }

    public static void main(String[] arguments) throws Exception {
        String token = required("MONITOR_API_TOKEN");
        if (token.length() < 24 || token.contains("\r") || token.contains("\n")) {
            throw new IllegalArgumentException("MONITOR_API_TOKEN은 줄바꿈 없는 24자 이상의 토큰이어야 합니다.");
        }
        String hostname = required("MONITOR_HOST_NAME");
        Path data = Path.of(env("MONITOR_DATA_DIR", "./data"));
        var mapper = JsonSupport.create();
        var collector = new LinuxResourceCollector(Path.of(env("MONITOR_PROC_ROOT", "/host/proc")),
                Path.of(env("MONITOR_DISK_ROOT", "/host/root")), hostname);
        var repository = new FileSettingsRepository(data.resolve("settings.json"), mapper);
        var sink = new FileEventSink(Path.of(env("MONITOR_WARN_DIR", "./warn")), data.resolve("logs"), mapper, 10 * 1024 * 1024);
        var service = new MonitorService(collector, repository, sink, new ThresholdEngine(), Clock.systemUTC());
        var scheduler = Executors.newSingleThreadScheduledExecutor(Thread.ofPlatform().name("host-sampler").factory());
        var webExecutor = Executors.newVirtualThreadPerTaskExecutor();
        MonitorHttpServer web;
        try {
            web = new MonitorHttpServer(new InetSocketAddress(env("MONITOR_HTTP_ADDRESS", "0.0.0.0"),
                    Integer.parseInt(env("MONITOR_HTTP_PORT", "8080"))), service, mapper, token, webExecutor);
        } catch (Exception exception) {
            scheduler.shutdownNow();
            webExecutor.close();
            throw exception;
        }
        AtomicBoolean closed = new AtomicBoolean();
        Runnable shutdown = () -> {
            if (!closed.compareAndSet(false, true)) {
                return;
            }
            scheduler.shutdown();
            web.close();
            try {
                if (!scheduler.awaitTermination(5, TimeUnit.SECONDS)) {
                    scheduler.shutdownNow();
                }
                webExecutor.shutdown();
                if (!webExecutor.awaitTermination(3, TimeUnit.SECONDS)) {
                    webExecutor.shutdownNow();
                }
            } catch (InterruptedException exception) {
                scheduler.shutdownNow();
                webExecutor.shutdownNow();
                Thread.currentThread().interrupt();
            }
        };
        Runtime.getRuntime().addShutdownHook(new Thread(shutdown, "monitor-shutdown"));
        try {
            scheduler.scheduleWithFixedDelay(() -> service.tick(System.nanoTime()), 0, 250, TimeUnit.MILLISECONDS);
            web.start();
            System.out.println("Resource monitor listening on port " + web.port() + "; warning directory=" + env("MONITOR_WARN_DIR", "./warn"));
        } catch (Exception exception) {
            shutdown.run();
            throw exception;
        }
    }

    private static String required(String name) {
        String value = System.getenv(name);
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException(name + " 환경 변수가 필요합니다.");
        }
        return value;
    }

    private static String env(String name, String fallback) {
        String value = System.getenv(name);
        return value == null || value.isBlank() ? fallback : value;
    }
}
