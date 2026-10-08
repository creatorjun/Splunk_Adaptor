// src/test/java/com/company/monitor/ArchitectureTest.java
package com.company.monitor;

import org.junit.jupiter.api.Test;
import java.nio.file.Files;
import java.nio.file.Path;
import static org.junit.jupiter.api.Assertions.*;

class ArchitectureTest {
    @Test
    void innerLayersDoNotImportInfrastructureOrFrameworks() throws Exception {
        for (String layer : new String[]{"domain", "application"}) {
            try (var sources = Files.walk(Path.of("src/main/java/com/company/monitor", layer))) {
                for (Path file : sources.filter(path -> path.toString().endsWith(".java")).toList()) {
                    String source = Files.readString(file);
                    assertFalse(source.contains("import com.company.monitor.infrastructure"), file.toString());
                    assertFalse(source.contains("import com.fasterxml"), file.toString());
                    assertFalse(source.contains("import com.sun.net.httpserver"), file.toString());
                    assertFalse(source.contains("import java.nio.file"), file.toString());
                    assertFalse(source.contains("Executors."), file.toString());
                }
            }
        }
    }
}
