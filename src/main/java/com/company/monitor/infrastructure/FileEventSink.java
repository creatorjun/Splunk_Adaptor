// src/main/java/com/company/monitor/infrastructure/FileEventSink.java
package com.company.monitor.infrastructure;

import com.company.monitor.application.MonitorPorts;
import com.company.monitor.domain.ResourceEvent;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;

public final class FileEventSink implements MonitorPorts.EventSink {
    private static final DateTimeFormatter FILE_TIME = DateTimeFormatter.ofPattern("uuuuMMdd'T'HHmmssSSS'Z'").withZone(ZoneOffset.UTC);
    private final Path warnDirectory;
    private final Path logDirectory;
    private final ObjectMapper mapper;
    private final long maxLogBytes;

    public FileEventSink(Path warnDirectory, Path logDirectory, ObjectMapper mapper, long maxLogBytes) throws IOException {
        this.warnDirectory = warnDirectory;
        this.logDirectory = logDirectory;
        this.mapper = mapper;
        this.maxLogBytes = maxLogBytes;
        Files.createDirectories(warnDirectory);
        Files.createDirectories(logDirectory);
        if (!Files.isWritable(warnDirectory) || !Files.isWritable(logDirectory)) {
            throw new IOException("warn 또는 로그 폴더에 쓰기 권한이 없습니다.");
        }
    }

    @Override
    public synchronized void write(ResourceEvent event) throws IOException {
        if (event.warning()) {
            String filename = FILE_TIME.format(event.occurredAt()) + "_" + event.eventId() + ".json";
            AtomicFiles.replace(warnDirectory.resolve(filename), mapper.writerWithDefaultPrettyPrinter().writeValueAsBytes(event));
        }
        String json = mapper.writeValueAsString(event);
        Path active = logDirectory.resolve("events.jsonl");
        if (Files.exists(active) && Files.size(active) >= maxLogBytes) {
            for (int index = 4; index >= 1; index--) {
                Path source = index == 1 ? active : logDirectory.resolve("events." + (index - 1) + ".jsonl");
                Path target = logDirectory.resolve("events." + index + ".jsonl");
                if (Files.exists(source)) {
                    Files.move(source, target, StandardCopyOption.REPLACE_EXISTING);
                }
            }
        }
        try (FileChannel channel = FileChannel.open(active, StandardOpenOption.CREATE, StandardOpenOption.WRITE, StandardOpenOption.APPEND)) {
            ByteBuffer buffer = ByteBuffer.wrap((json + "\n").getBytes(StandardCharsets.UTF_8));
            while (buffer.hasRemaining()) {
                channel.write(buffer);
            }
            channel.force(true);
        }
        System.out.println(json);
    }
}
