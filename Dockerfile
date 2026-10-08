# Dockerfile
FROM maven:3.9.11-eclipse-temurin-21 AS build
WORKDIR /workspace
COPY pom.xml .
RUN mvn -B dependency:go-offline
COPY src ./src
RUN mvn -B verify

FROM eclipse-temurin:21-jre-ubi9-minimal
WORKDIR /app
RUN mkdir -p /data /warn && chown -R 10001:10001 /data /warn
COPY --from=build /workspace/target/resource-monitor.jar /app/resource-monitor.jar
USER 10001:10001
EXPOSE 8080
ENTRYPOINT ["java", "-XX:MaxRAMPercentage=65.0", "-jar", "/app/resource-monitor.jar"]
