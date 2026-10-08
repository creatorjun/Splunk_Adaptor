// src/main/java/com/company/monitor/bootstrap/HealthCheck.java
package com.company.monitor.bootstrap;

import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.time.Duration;

public final class HealthCheck {
    private HealthCheck() { }

    public static void main(String[] arguments) {
        try (HttpClient client = HttpClient.newBuilder().connectTimeout(Duration.ofSeconds(2)).build()) {
            HttpRequest request = HttpRequest.newBuilder(URI.create("http://127.0.0.1:8080/health"))
                    .timeout(Duration.ofSeconds(3)).GET().build();
            int code = client.send(request, HttpResponse.BodyHandlers.discarding()).statusCode();
            if (code != 200) {
                System.exit(1);
            }
        } catch (Exception exception) {
            System.exit(1);
        }
    }
}
