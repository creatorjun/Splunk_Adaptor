<!-- README.md -->
# Oracle Linux 시스템 리소스 모니터

Java 21로 작성한 Docker 기반 호스트 모니터링 프로그램입니다. CPU, 메모리, 지정한 디스크 경로의 사용량이 사용자 임계치에 도달하면 **이 프로젝트의 `warn/` 폴더에 개별 JSON 파일을 생성**합니다. 설정은 웹 화면에서 변경하며 재시작 없이 적용됩니다.

## 프로젝트 구조

```text
Splunk_Adaptor/
├── pom.xml
├── Dockerfile
├── compose.yaml
├── .env.example
├── .dockerignore
├── .gitignore
├── .gitattributes
├── README.md
├── scripts/
│   ├── prepare.sh
│   └── publish.sh
├── warn/
├── src/main/java/com/company/monitor/
│   ├── domain/
│   │   ├── ThresholdRule.java
│   │   ├── MonitorSettings.java
│   │   ├── ResourceSnapshot.java
│   │   ├── ResourceEvent.java
│   │   └── ThresholdEngine.java
│   ├── application/
│   │   ├── MonitorPorts.java
│   │   └── MonitorService.java
│   ├── infrastructure/
│   │   ├── JsonSupport.java
│   │   ├── AtomicFiles.java
│   │   ├── FileSettingsRepository.java
│   │   ├── LinuxResourceCollector.java
│   │   ├── FileEventSink.java
│   │   └── MonitorHttpServer.java
│   └── bootstrap/
│       ├── Main.java
│       └── HealthCheck.java
├── src/main/resources/web/
│   ├── index.html
│   ├── app.css
│   └── app.js
└── src/test/java/com/company/monitor/
    ├── ArchitectureTest.java
    ├── domain/
    │   ├── ThresholdEngineTest.java
    │   └── MonitorSettingsTest.java
    ├── application/
    │   └── MonitorServiceTest.java
    └── infrastructure/
        ├── LinuxResourceCollectorTest.java
        ├── FilePersistenceTest.java
        └── MonitorHttpServerTest.java
```

## Oracle Linux에서 실행

Oracle Linux의 Linux Docker Engine과 Docker Compose 플러그인을 사용합니다. 호스트에 Java나 Maven을 별도 설치할 필요가 없습니다. 이미지 빌드 과정에서 Java 21, 의존성 다운로드, 컴파일과 테스트를 수행합니다. 실제 사용 중인 Oracle Linux 버전·커널·SELinux 정책에서 마지막 실행 확인을 진행해야 합니다.

프로젝트 디렉터리에서 실행합니다.

```bash
bash scripts/prepare.sh
docker compose up -d --build
docker compose ps
```

준비 스크립트는 `.env`가 없을 때만 256비트 임의 관리 토큰과 호스트명을 저장합니다. 프로젝트의 `warn/` 폴더를 만들고 컨테이너 사용자 UID/GID `10001:10001`에 쓰기 권한을 부여합니다. 일반 계정으로 실행하면 폴더 권한 변경에 `sudo`를 사용합니다. 기존 `.env`는 덮어쓰지 않습니다. Docker 명령 실행 권한이 없는 환경에서는 해당 명령에 `sudo`를 사용합니다.

관리 화면: [http://127.0.0.1:8080](http://127.0.0.1:8080). `.env`의 `MONITOR_API_TOKEN` 값을 입력하여 연결하세요. 토큰은 브라우저 메모리에만 보관하며, 페이지를 다시 열면 재입력합니다.

```bash
cat .env
ls -lh warn/
docker compose logs -f monitor
```

외부 PC에서 접속하려면 SSH 터널을 이용할 수 있습니다.

```bash
ssh -L 8080:127.0.0.1:8080 user@oracle-linux-host
```

이후 접속 PC에서 `http://127.0.0.1:8080`을 엽니다. 외부 공개가 필요할 경우 `.env`의 바인드 주소와 방화벽을 운영 환경에 맞게 지정하고 HTTPS 리버스 프록시를 구성합니다. 기본값은 호스트의 localhost 바인딩입니다.

## 저장 위치

| 내용 | Oracle Linux 호스트 | 컨테이너 |
|---|---|---|
| 임계치 도달·재알림 JSON | **프로젝트/warn/** | `/warn/` |
| 저장된 임계치 설정 | Docker `monitor-data` 볼륨 | `/data/settings.json` |
| 도달·복구 이벤트 통합 로그 | Docker `monitor-data` 볼륨 | `/data/logs/events.jsonl` |

`MONITOR_WARN_HOST_DIR=./warn`이 기본값입니다. 프로젝트를 이동하면 그 프로젝트 위치를 기준으로 새 `warn/` 폴더를 사용합니다. 다른 경로를 지정하는 경우 해당 경로를 미리 만들고 UID/GID `10001:10001`의 쓰기 권한을 부여해야 합니다. 준비 스크립트는 기본 `./warn`만 준비합니다.

파일 이름: `20261008T090000000Z_<eventId>.json`. 모든 시각은 UTC ISO 8601로 기록하며 웹 화면은 브라우저의 현지 시각을 표시합니다. JSON은 임시 파일에 완전히 기록하고 flush한 후 같은 디렉터리에서 원자적으로 이름을 변경합니다. 외부 수집기는 `warn/*.json`만 대상으로 삼으면 됩니다. `.tmp` 파일은 수집 대상에서 제외하세요.

`warn/` JSON은 자동 삭제하지 않습니다. 보관·수집·아카이브 주기는 운영 정책에 따라 관리합니다. 통합 이벤트 로그는 파일당 약 10MiB에서 순환하며 현재 파일 1개와 백업 4개를 유지합니다. 컨테이너 표준 출력 로그도 Compose에서 순환 저장합니다.

복구 이벤트는 통합 로그와 화면에 기록합니다. `warn/`에는 도달·재알림 이벤트만 저장합니다. CPU와 메모리가 동시에 도달하면 자원별 파일이 각각 생성되며 각 파일에 당시 전체 시스템 측정값이 포함됩니다.

## JSON 형식

아래는 형식 설명용 예시입니다.

```json
{
  "schemaVersion": 1,
  "eventId": "92e39980-3835-4285-aaab-16e488d425c4",
  "occurredAt": "2026-10-08T09:00:00Z",
  "type": "THRESHOLD_REACHED",
  "resource": "cpu",
  "usedPercent": 82.5,
  "thresholdPercent": 80.0,
  "snapshot": {
    "capturedAt": "2026-10-08T09:00:00Z",
    "host": "oracle-linux-host",
    "cpuPercent": 82.5,
    "memory": {
      "totalBytes": 8589934592,
      "availableBytes": 2147483648,
      "usedBytes": 6442450944,
      "usedPercent": 75.0
    },
    "disks": [
      {
        "path": "/",
        "totalBytes": 107374182400,
        "freeBytes": 21474836480,
        "usedBytes": 85899345920,
        "usedPercent": 80.0
      }
    ],
    "errors": []
  }
}
```

`resource`는 `cpu`, `memory`, `disk:/`, `disk:/var` 등의 값입니다. 일부 자원을 읽지 못하면 해당 값은 `null` 또는 디스크 목록에서 제외되며 `snapshot.errors`에 오류를 기록합니다. 측정 실패를 사용량 0으로 대체하지 않습니다. 다른 정상 자원의 임계치 판정은 계속 진행합니다.

## 판정과 실시간 설정

| 설정 | 기본값 | 범위 / 동작 |
|---|---|---|
| CPU 임계치 | 80% | 사용량 ≥ 임계치 |
| Memory 임계치 | 85% | 사용량 ≥ 임계치 |
| Disk 임계치 | 90% | 설정 경로별 판정 |
| 측정 주기 | 5초 | 1~300초 |
| 연속 도달 횟수 | 1회 | 1~60회 |
| 복구 여유폭 | 5%p | 사용량 < 임계치 − 여유폭이면 복구 |
| 재알림 주기 | 300초 | 0~86400초; 0이면 재알림 해제 |
| 디스크 경로 | `/` | 최대 32개, 한 줄당 호스트 절대 경로 |

예를 들어 CPU 임계치가 80%, 여유폭이 5%p이면 80% 이상에서 도달 이벤트를 만들고 75% 미만에서 복구됩니다. 복구 전 지속 초과 상태는 재알림 주기에 맞춰 추가 JSON을 생성합니다. 여유폭 구간에서는 경보를 유지하지만 도달 임계치 미만이라면 재알림을 생성하지 않습니다.

설정 저장은 디스크에 성공한 뒤 메모리 설정을 교체합니다. 250ms 간격의 백그라운드 실행 루프가 다음 측정을 요청하므로 종전 측정 주기가 길어도 설정 반영을 위해 그 주기를 기다리지 않습니다. 실제 반영 시각은 호스트 파일 읽기·쓰기 완료 시간에 영향을 받습니다. 연속 도달 횟수가 2 이상이면 새 설정으로 해당 횟수만큼 측정한 뒤 도달 이벤트를 생성합니다. 웹 화면은 1초마다 최신 상태를 조회합니다.

여러 화면에서 동시에 수정하면 설정 버전으로 충돌을 감지하고 HTTP 409를 반환합니다. 이미 편집 중인 값은 자동 갱신으로 덮어쓰지 않습니다. 자원 알림을 해제하면 기존 경보 상태를 해제합니다. 프로세스 재시작 시 경보 상태와 최근 화면 이벤트는 초기화되며, 지속 도달 중인 자원은 다시 이벤트를 생성할 수 있습니다. JSON과 통합 로그는 보존됩니다.

저장 실패 시 화면과 상태 API에 오류를 표시하고 같은 eventId로 재시도합니다. 재시도는 측정 주기를 따릅니다. JSON 파일 이름이 동일하므로 재시도가 별도의 경고 파일을 무한히 만들지 않습니다. 통합 로그에는 저장 단계의 부분 실패로 중복 행이 생길 수 있으므로 외부 수집 시 `eventId`로 중복을 제거할 수 있습니다. 강제 종료나 전원 장애 시 아직 저장되지 않은 메모리상의 대기 이벤트는 보존되지 않습니다.

## 호스트 측정 방식

CPU는 읽기 전용으로 연결한 호스트 `/proc/stat`의 전체 CPU 누적 카운터 차이를 이용합니다. idle과 iowait을 유휴 시간으로 취급하고 guest 시간을 중복 합산하지 않습니다. 첫 측정은 기준 카운터를 만들며 약 1초 후 사용률을 계산합니다. 메모리 사용량은 호스트 `/proc/meminfo`의 `MemTotal − MemAvailable`로 계산합니다. 디스크는 호스트 루트 마운트 아래에서 지정한 경로의 파일시스템 전체·미할당 공간을 조회하고 `(total − free) / total × 100`으로 계산합니다. 예약 블록을 가용 공간에서 제외하는 `df`의 표시 퍼센트와 차이가 있을 수 있습니다.

호스트에 디스크를 새로 마운트하거나 마운트를 변경하면 컨테이너를 재생성하여 호스트 마운트 구성을 다시 반영하세요. 호스트 루트 밖으로 해석되는 절대 심볼릭 링크는 오류로 표시합니다. 해당 링크의 실제 호스트 경로를 직접 설정하면 됩니다.

Compose는 `/proc`와 `/`를 읽기 전용으로 마운트합니다. Oracle Linux의 SELinux 호스트 파일 접근을 위해 **이 컨테이너에 한해** `label=disable`을 적용합니다. 호스트 전체의 SELinux 모드는 변경하지 않으며 시스템 디렉터리에 `:z` 또는 `:Z`로 레이블을 바꾸지 않습니다. 프로세스는 UID 10001, capability 제거, 읽기 전용 컨테이너 파일시스템으로 실행합니다.

Linux 커널 5.12 이전에서는 Docker의 읽기 전용 재귀 bind mount 하위 마운트가 읽기·쓰기로 연결될 수 있습니다. 이 프로그램의 수집기는 읽기 동작만 수행하지만 하위 마운트까지 읽기 전용 격리가 필요하면 5.12 이상 커널을 사용하거나 모니터링 경로별 독립 읽기 전용 마운트 구성으로 조정해야 합니다. Docker Desktop에서의 테스트는 Linux VM 자원을 읽으므로 Windows 호스트나 Oracle Linux 실서버 사용량 검증과 구분해야 합니다.

## API

관리 API는 `Authorization: Bearer <MONITOR_API_TOKEN>` 헤더가 필요합니다. `/health`는 Docker 상태 확인용으로 인증 없이 응답합니다.

| 메서드 | 경로 | 기능 |
|---|---|---|
| GET | `/api/status` | 설정, 측정값, 경보 상태, 최근 100개 이벤트, 오류 |
| PUT | `/api/settings` | 검증·영속 저장 후 즉시 적용 |
| GET | `/health` | 정상 수집 200, 준비·수집·파일 저장 오류 503 |

설정 요청은 부분 수정이 아닌 전체 설정과 `expectedVersion`을 전달합니다.

```json
{
  "expectedVersion": 1,
  "settings": {
    "sampleIntervalSeconds": 5,
    "consecutiveSamples": 1,
    "repeatIntervalSeconds": 300,
    "cpu": { "enabled": true, "thresholdPercent": 80, "hysteresisPercent": 5 },
    "memory": { "enabled": true, "thresholdPercent": 85, "hysteresisPercent": 5 },
    "disk": { "enabled": true, "thresholdPercent": 90, "hysteresisPercent": 5 },
    "diskPaths": ["/", "/var"]
  }
}
```

## 개발 및 검증

Java 21 이상과 Maven 3.9 이상을 설치한 개발 환경에서 실행합니다. Java 프로젝트의 의존성은 `requirements.txt` 대신 **`pom.xml`**에서 관리합니다. 런타임 의존성은 Jackson JSON 및 Java Time 모듈이며 테스트에는 JUnit을 사용합니다.

```bash
mvn verify
```

실행 가능한 단일 JAR: `target/resource-monitor.jar`.

호스트 Linux에서 JAR를 직접 실행하려면 다음과 같이 경로를 지정합니다.

```bash
export MONITOR_API_TOKEN=$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n')
export MONITOR_HOST_NAME=$(hostname)
export MONITOR_PROC_ROOT=/proc
export MONITOR_DISK_ROOT=/
export MONITOR_WARN_DIR=./warn
export MONITOR_HTTP_ADDRESS=127.0.0.1
java -jar target/resource-monitor.jar
```

도메인에는 순수 임계치 정책과 데이터 모델, 애플리케이션에는 수집·저장 포트와 처리 흐름만 둡니다. Linux 파일시스템, JSON 직렬화, HTTP, 영속 저장은 외부 어댑터에 구현합니다. bootstrap이 객체 구성과 스레드·HTTP 서버의 시작·종료를 소유합니다. 파일과 채널은 try-with-resources로 닫습니다. 코드 주석은 파일 첫 줄의 경로/파일명만 작성합니다.

테스트는 경계값 도달, 연속 확인, 복구 여유폭, 재알림, 측정 누락, 디스크별 판정, 설정 실패·버전 충돌, 저장 실패 후 동일 ID 재시도, Linux 카운터 계산, JSON 저장·순환 로그, API 인증·검증, 아키텍처 의존 방향을 확인합니다. 실제 Oracle Linux 호스트의 마운트·사용량·SELinux 접근은 해당 서버에서 추가 확인해야 합니다.

GitHub 저장소: [creatorjun/Splunk_Adaptor](https://github.com/creatorjun/Splunk_Adaptor). Git 자격 증명과 커밋 작성자가 설정된 환경에서는 아래 스크립트로 변경 사항을 커밋하고 푸시합니다. `.env`, 경고 파일, 로컬 검증 자료와 빌드 산출물은 `.gitignore`로 제외합니다.

```bash
bash scripts/publish.sh "Update resource monitor"
```

## 참고 문서

- [Linux kernel: /proc 파일시스템과 CPU·메모리 통계](https://www.kernel.org/doc/html/latest/filesystems/proc.html)
- [Docker: bind mount, 재귀 읽기 전용과 SELinux 동작](https://docs.docker.com/engine/storage/bind-mounts/)
- [Java 21: HttpServer](https://docs.oracle.com/en/java/javase/21/docs/api/jdk.httpserver/com/sun/net/httpserver/HttpServer.html)
