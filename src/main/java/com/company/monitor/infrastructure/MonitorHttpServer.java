// src/main/java/com/company/monitor/infrastructure/MonitorHttpServer.java
package com.company.monitor.infrastructure;

import com.company.monitor.application.MonitorService;
import com.company.monitor.domain.MonitorSettings;
import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.time.Duration;
import java.time.Instant;
import java.util.Map;
import java.util.concurrent.Executor;

public final class MonitorHttpServer implements AutoCloseable {
    public record SettingsRequest(long expectedVersion, MonitorSettings settings) { }

    private final HttpServer server;
    private final MonitorService service;
    private final ObjectMapper mapper;
    private final byte[] token;

    public MonitorHttpServer(InetSocketAddress address, MonitorService service, ObjectMapper mapper,
                             String apiToken, Executor executor) throws IOException {
        this.service = service;
        this.mapper = mapper;
        this.token = ("Bearer " + apiToken).getBytes(StandardCharsets.UTF_8);
        this.server = HttpServer.create(address, 64);
        server.setExecutor(executor);
        server.createContext("/", this::handle);
    }

    public void start() {
        server.start();
    }

    public int port() {
        return server.getAddress().getPort();
    }

    private void handle(HttpExchange exchange) throws IOException {
        try (exchange) {
            exchange.getResponseHeaders().set("X-Content-Type-Options", "nosniff");
            exchange.getResponseHeaders().set("Cache-Control", "no-store");
            exchange.getResponseHeaders().set("Content-Security-Policy",
                    "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
            String path = exchange.getRequestURI().getPath();
            String method = exchange.getRequestMethod();
            if ("/health".equals(path) && "GET".equals(method)) {
                MonitorService.Status status = service.status();
                boolean ready = status.lastError() == null && status.snapshot() != null
                        && status.snapshot().errors().isEmpty() && status.snapshot().cpuPercent() != null
                        && status.lastCycleAt() != null
                        && Duration.between(status.lastCycleAt(), Instant.now()).getSeconds() <= status.settings().sampleIntervalSeconds() * 2L + 5;
                sendJson(exchange, ready ? 200 : 503, Map.of("status", ready ? "UP" : "DEGRADED"));
                return;
            }
            if (path.startsWith("/api/")) {
                String supplied = exchange.getRequestHeaders().getFirst("Authorization");
                if (supplied == null || !MessageDigest.isEqual(token, supplied.getBytes(StandardCharsets.UTF_8))) {
                    sendJson(exchange, 401, Map.of("error", "관리 토큰을 확인하세요."));
                    return;
                }
                if ("/api/status".equals(path) && "GET".equals(method)) {
                    sendJson(exchange, 200, service.status());
                } else if ("/api/settings".equals(path) && "PUT".equals(method)) {
                    updateSettings(exchange);
                } else {
                    sendJson(exchange, 404, Map.of("error", "요청 경로 또는 메서드를 확인하세요."));
                }
                return;
            }
            if (!"GET".equals(method)) {
                sendJson(exchange, 405, Map.of("error", "GET 요청만 지원합니다."));
                return;
            }
            String resource = switch (path) {
                case "/", "/index.html" -> "web/index.html";
                case "/app.css" -> "web/app.css";
                case "/app.js" -> "web/app.js";
                default -> null;
            };
            if (resource == null) {
                sendJson(exchange, 404, Map.of("error", "파일을 찾을 수 없습니다."));
                return;
            }
            try (var input = getClass().getClassLoader().getResourceAsStream(resource)) {
                if (input == null) {
                    sendJson(exchange, 404, Map.of("error", "화면 파일을 찾을 수 없습니다."));
                    return;
                }
                String contentType = resource.endsWith(".css") ? "text/css" : resource.endsWith(".js") ? "text/javascript" : "text/html";
                send(exchange, 200, contentType + "; charset=utf-8", input.readAllBytes());
            }
        } catch (IOException exception) {
            System.err.println("HTTP exchange failed: " + exception.getMessage());
        }
    }

    private void updateSettings(HttpExchange exchange) throws IOException {
        String type = exchange.getRequestHeaders().getFirst("Content-Type");
        if (type == null || !type.toLowerCase(java.util.Locale.ROOT).startsWith("application/json")) {
            sendJson(exchange, 415, Map.of("error", "application/json 형식으로 요청하세요."));
            return;
        }
        byte[] content = exchange.getRequestBody().readNBytes(16385);
        if (content.length > 16384) {
            sendJson(exchange, 413, Map.of("error", "설정 요청은 16KB 이하여야 합니다."));
            return;
        }
        try {
            SettingsRequest request = mapper.readValue(content, SettingsRequest.class);
            if (request.settings() == null) {
                throw new IllegalArgumentException("설정이 필요합니다.");
            }
            service.updateSettings(request.settings(), request.expectedVersion());
            sendJson(exchange, 200, service.status());
        } catch (JsonProcessingException | IllegalArgumentException exception) {
            sendJson(exchange, 400, Map.of("error", "설정값 또는 JSON 형식을 확인하세요."));
        } catch (IllegalStateException exception) {
            sendJson(exchange, 409, Map.of("error", exception.getMessage()));
        } catch (IOException exception) {
            sendJson(exchange, 500, Map.of("error", "설정 저장에 실패했습니다. 데이터 폴더의 권한과 여유 공간을 확인하세요."));
        }
    }

    private void sendJson(HttpExchange exchange, int status, Object value) throws IOException {
        send(exchange, status, "application/json; charset=utf-8", mapper.writeValueAsBytes(value));
    }

    private static void send(HttpExchange exchange, int status, String type, byte[] bytes) throws IOException {
        exchange.getResponseHeaders().set("Content-Type", type);
        exchange.sendResponseHeaders(status, bytes.length);
        exchange.getResponseBody().write(bytes);
    }

    @Override
    public void close() {
        server.stop(1);
    }
}
