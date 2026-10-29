<!-- README.md -->
# Resource Monitor

Oracle Linux 8/9용 C++23 시스템 모니터링 데몬입니다. 기본 60초마다 CPU, 메모리, 마운트된 로컬 디스크 파일시스템을 수집하고 매 수집 주기의 전체 결과를 OS 시간대와 UTC 오프셋을 포함한 파일명으로 저장합니다. 예를 들어 OS가 Asia/Seoul이면 `/Monitor/20261019T123400+0900.json`에 저장합니다. `is_analyzer`에 맞는 Java `-DAPP` 프로세스의 실행 여부도 같은 JSON에 기록하며, 1.6.0부터 역할별 대상 목록을 설정 파일에서 재시작 없이 변경할 수 있습니다. 분석 장비에서는 `CloudESM.SUM_COLLECT_INFO_1DAY`의 일별 수집 건수 변동과 `CloudESM.SUM_COLLECT_INFO`의 장비별 로그 무수신 상태를 함께 기록합니다. 1.7.0부터 무수신 감시 대상 IP는 매 DB 조회에서 자동으로 발견합니다. 1.8.0부터 수집 장비의 처리 폴더를 60초마다 검사하여 `.tmp`·`.done` 파일 개수가 설정한 임계치 이상인 항목만 결과에 추가합니다. `/etc/resource-monitor/settings.json`에서 수집 주기, 저장 경로, 장비 역할, DAPP 목록, 임계치, 분석 기준과 MySQL 연결을 관리합니다. DB 사용자명과 비밀번호는 `/etc/resource-monitor/secrets/mysql.json`으로 분리합니다. 리소스 임계치는 경고 판정에 적용하며 모든 수집 주기의 파일을 남깁니다. 1.10.0은 모니터링 JSON 자동 삭제 여부와 보존 시간을 설정으로 변경하고, 일별 건수와 무수신 조회의 DB 연결·작업 스레드를 분리합니다. 결과 JSON에는 임계값 자체를 기록하지 않으며 임계값은 `settings.json`에서 관리합니다.

현재 버전은 **1.11.0**입니다. 분석 서버에서 지정한 시간 구간의 장비별 로그 수집량을 `log_volume`으로 추가 기록합니다. 기본값은 최근 완료된 5분 구간이며, 매일 정한 현지 시각에 전날 하루를 집계하도록 변경할 수 있습니다. 기존 일별 증감 분석과 무수신 감시는 유지합니다.

문서 말미의 [1.8.0→1.10.0 상세 이전 가이드](#upgrade-from-1-8)는 당시 배포 파일을 사용하는 보관용 절차입니다. 현재 1.11.0의 빌드·설정·기능 설명과 구분하십시오.

## 구성

| 위치 | 책임 |
| --- | --- |
| `src/domain` | 측정 모델, 계산식, 임계치 규칙, 상태 집계 |
| `src/application` | 수집·일반 설정 적용·유효 설정 유지·DB 연결 전환 조정·프로세스 대상 선택·저장·보존 유스케이스와 포트 |
| `src/infrastructure/linux` | procfs의 리소스·Java 프로세스, mountinfo, 파일시스템 용량 수집과 비동기 처리 파일 검사 |
| `src/infrastructure/config` | 사용자 설정·MySQL 계정 JSON 파싱·검증과 설정 포트 구현 |
| `src/infrastructure/database` | 일별 건수·무수신·구간 수집량 각각의 MySQL 연결 준비·교체·연결 확인·재시도와 비동기 조회 |
| `src/infrastructure/persistence` | 저장 경로 검증·잠금·전환, JSON 직렬화와 원자적 파일 저장 |
| `src/runtime` | 단조 시계 수집·설정 확인 타이머, 종료 신호 |
| `src/main.cpp` | 구성 객체 생성과 의존성 주입 |
| `tests` | 계산·설정·저장·실행 통합·의존성 검사 |
| `packaging` | systemd 서비스, RPM 명세, Docker 빌드 환경 |

Domain은 Linux와 JSON을 참조하지 않습니다. Application은 자신의 포트와 Domain에만 의존합니다. 운영체제 호출과 JSON 라이브러리는 외부 계층에 있습니다. 테스트에서 의존성 방향과 작성한 소스의 파일 상단 경로 주석 규칙을 검사합니다.

Java 프로세스 모니터링의 의존성은 다음 파일로 나눕니다. Application은 현재 적용된 설정으로 대상 목록을 선택하고, Linux 어댑터는 프로세스 관측을 담당합니다. JSON 저장기는 같은 수집 결과에 관측값을 직렬화합니다.

```text
src/
  application/
    ProcessMonitorSettings.hpp
    JavaProcessMonitor.hpp
    JavaProcessMonitor.cpp
    ports/
      IJavaProcessMonitor.hpp
      IJavaProcessReader.hpp
  infrastructure/
    linux/
      ProcJavaProcessReader.hpp
      ProcJavaProcessReader.cpp
    persistence/
      JsonSnapshotStore.cpp
```

## 빌드

필수 도구는 C++23 및 `std::expected`를 지원하는 GCC Toolset 15, CMake 3.20 이상, Make, pkg-config와 MariaDB Connector/C 개발 패키지입니다. MySQL 연결에는 배포판의 MariaDB Connector/C 공유 라이브러리를 사용합니다. 테스트에는 Python 3 표준 라이브러리를 사용합니다. 런타임에는 Python이 필요하지 않습니다. JSON 라이브러리 nlohmann/json 3.12.0은 CMake가 공식 배포본을 SHA-256 검증 후 가져옵니다.

Oracle Linux의 활성 AppStream 저장소에 해당 패키지가 있어야 합니다. OL8/9 각각의 대상 배포판에서 별도로 빌드하고 설치하십시오.

```bash
sudo dnf install -y gcc-toolset-15-gcc-c++ cmake make python3 pkgconf-pkg-config mariadb-connector-c-devel
source /opt/rh/gcc-toolset-15/enable
cmake --preset release
cmake --build --preset release --parallel 2
ctest --preset release
```

폐쇄망에서는 공식 `json.tar.xz`를 미리 받아 해제하고 다음처럼 지정할 수 있습니다. SHA-256은 `42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa`입니다.

```bash
cmake --preset release -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=/absolute/path/to/json
```

설치된 CMake 패키지를 사용하는 경우 정확히 nlohmann_json 3.12.0이어야 합니다. 자동 다운로드를 막으려면 `-DMONITOR_FETCH_JSON=OFF`를 함께 지정합니다.

## 소스 설치와 서비스 시작

```bash
sudo cmake --install build/release
getent group resource-monitor >/dev/null || sudo groupadd --system resource-monitor
getent passwd resource-monitor >/dev/null || sudo useradd --system --gid resource-monitor --home-dir /nonexistent --shell /sbin/nologin resource-monitor
sudo install -d -m 0750 -o resource-monitor -g resource-monitor /Monitor
sudo install -d -m 0750 -o root -g resource-monitor /etc/resource-monitor/secrets
sudo /usr/sbin/resource-monitor --check-config
sudo systemctl daemon-reload
sudo systemctl enable --now resource-monitor
systemctl status resource-monitor
journalctl -u resource-monitor -f
```

기존 `settings.json`은 CMake 재설치와 RPM 업그레이드 시 보존합니다. 새 파일이 없고 이전 `thresholds.json`만 있으면 기존 내용을 `settings.json`으로 복사합니다. 기존 파일의 값이 잘못되어 있어도 기본값으로 바꾸지 않습니다. 기본 예제는 `/usr/share/resource-monitor/settings.json`에 별도로 설치됩니다. MySQL 계정 예제는 `/usr/share/resource-monitor/mysql.example.json`이며 실제 계정 파일은 설치하지 않으므로 기존 `secrets/mysql.json`을 덮어쓰지 않습니다. `/Monitor`는 로컬 파일시스템에 두고 서비스 계정이 쓰기 가능해야 합니다. 설치 명령은 관리자 권한이 필요하지만 데몬은 전용 계정으로 실행됩니다.

SELinux가 Enforcing인 실제 서버에서 파일 쓰기와 디렉터리 접근을 검증하십시오. 이 패키지는 SELinux를 해제하거나 별도 정책을 자동 적용하지 않습니다. 모니터링 대상 경로를 서비스 계정이 탐색할 수 없으면 해당 항목에 접근 오류가 기록됩니다. systemd 단위는 마운트 네임스페이스를 격리하는 옵션을 사용하지 않습니다.

## 사용자 설정 수정

`/etc/resource-monitor/settings.json`의 예시입니다.

```json
{
  "schema_version": 1,
  "is_analyzer": false,
  "analyzer": {
    "collect_count_change_percent": 10.0,
    "comparison_day": "yesterday"
  },
  "log_volume": {
    "mode": "interval",
    "interval_seconds": 300,
    "daily_at": "00:10"
  },
  "log_silence": {
    "default_timeout_seconds": 300,
    "overrides": []
  },
  "processing_files": {
    "thresholds": {
      "collector": { "tmp_count": 100, "done_count": 100 },
      "normalize": { "tmp_count": 100, "done_count": 100 }
    }
  },
  "process_monitor": {
    "analyzer": [
      "SearchEngine", "Normalizer", "CollectorForAgent", "CDMEngine",
      "ShovelServer", "AnalyzeEngine", "AnalyzeAssistantEngine", "SoarTicketMain",
      "SoarPlaybookManager", "pmsServer", "pmsClient", "ProcessMonitor"
    ],
    "collector": [
      "SearchEngine", "Reducer", "Normalizer", "CloudESMCollector",
      "CollectorForAgent", "CDMEngine", "pmsClient", "ProcessMonitor"
    ]
  },
  "collection": {
    "interval_seconds": 60,
    "output_directory": "/Monitor"
  },
  "retention": {
    "enabled": true,
    "max_age_seconds": 3600
  },
  "thresholds": {
    "cpu_usage_percent": 80.0,
    "memory_usage_percent": 85.0,
    "storage_usage_percent": 90.0
  },
  "mysql": {
    "host": "127.0.0.1",
    "port": 3306,
    "database": "monitoring",
    "credentials_file": "/etc/resource-monitor/secrets/mysql.json",
    "connect_timeout_seconds": 3,
    "read_timeout_seconds": 3,
    "write_timeout_seconds": 3,
    "tls_mode": "verify_identity",
    "tls_ca_file": "/etc/resource-monitor/secrets/mysql-ca.pem"
  }
}
```

| 설정 | 허용 값과 기본값 | 변경 적용 시점 |
| --- | --- | --- |
| `is_analyzer` | JSON 불리언 `true` 또는 `false`, 생략 시 기본 `false` | 다음 설정 적용 후 DB 연결·쿼리 허용 여부 변경, 다음 수집부터 Java 프로세스 대상 변경 |
| `analyzer.collect_count_change_percent` | 0 초과 1,000,000 이하의 유한한 숫자, 기본 10.0% | 다음 설정 확인 후 분석부터, DB 재연결 없음 |
| `analyzer.comparison_day` | `yesterday` 또는 `today`, 기본 `yesterday` | 다음 설정 확인 후 새 날짜 쌍 조회 요청 |
| `log_volume.mode` | `interval` 또는 `daily`, 기본 `interval` | 다음 설정 확인 후 해당 방식의 최근 완료 구간 요청 |
| `log_volume.interval_seconds` | 60~86400의 정수이며 60의 배수, 기본 300초 | `interval` 방식의 집계 길이·예약 경계 변경 |
| `log_volume.daily_at` | 현지 시각 `HH:MM`, `00:00`~`23:59`, 기본 `00:10` | `daily` 방식의 전날 집계 예약 시각 변경 |
| `process_monitor.analyzer` | `is_analyzer=true`일 때 감시할 DAPP 이름 배열, 기본값은 아래 분석 장비 목록 | 다음 설정 적용 후 수집부터, DB 재연결 없음 |
| `process_monitor.collector` | `is_analyzer=false`일 때 감시할 DAPP 이름 배열, 기본값은 아래 수집 장비 목록 | 다음 설정 적용 후 수집부터 |
| `log_silence.default_timeout_seconds` | 60~604800 정수, 기본 300초 | 다음 수집부터 모든 DB 장비의 기본 무수신 제한 변경, DB 재연결 없음 |
| `log_silence.overrides` | 최대 256개 장비별 `{eqp_ip, timeout_seconds}` 예외 설정, 기본 `[]` | 다음 수집부터 해당 장비의 제한만 변경; 감시 대상은 항상 DB에서 자동 발견 |
| `processing_files.thresholds.collector.tmp_count` | 0~1,000,000,000,000 정수, 기본 100개 | 다음 수집부터 collector 하위 `.tmp` 합계에 적용 |
| `processing_files.thresholds.collector.done_count` | 0~1,000,000,000,000 정수, 기본 100개 | 다음 수집부터 collector 하위 `.done` 합계에 적용 |
| `processing_files.thresholds.normalize.tmp_count` | 0~1,000,000,000,000 정수, 기본 100개 | 다음 수집부터 normalize 하위 `.tmp` 합계에 적용 |
| `processing_files.thresholds.normalize.done_count` | 0~1,000,000,000,000 정수, 기본 100개 | 다음 수집부터 normalize 하위 `.done` 합계에 적용 |
| `collection.interval_seconds` | 1~86400의 정수, 기본 60초 | 다음 설정 확인 후 새 주기 시작 |
| `collection.output_directory` | 제어문자가 없는 절대 경로, 기본 `/Monitor` | 다음 설정 확인 후 저장부터 |
| `retention.enabled` | JSON 불리언, 기본 `true` | 다음 설정 확인 후 정리부터; `false`이면 오래된 JSON도 삭제하지 않음 |
| `retention.max_age_seconds` | 60~31536000의 정수, 기본 3600초 | 다음 60초 정리 시 새 보존 시간 적용 |
| `thresholds.cpu_usage_percent` | 0~100의 숫자 | 다음 설정 확인 후 수집부터 |
| `thresholds.memory_usage_percent` | 0~100의 숫자 | 다음 설정 확인 후 수집부터 |
| `thresholds.storage_usage_percent` | 0~100의 숫자 | 다음 설정 확인 후 수집부터 |

세 리소스 임계치는 필수이며 유한한 숫자여야 합니다. 리소스 임계치의 0은 항상 경고를 발생시킵니다. `collection`, `retention`, `analyzer`, `process_monitor`, `log_silence`, `log_volume`, `processing_files` 또는 그 하위 설정을 생략하면 해당 기본값을 사용하므로 이전 설정 JSON도 읽을 수 있습니다. 분석 서버에서 `log_volume`을 생략하면 기본 5분 구간 집계가 활성화됩니다. `retention`을 생략하면 이전 1.9.0과 같은 자동 삭제 활성화·1시간 보존을 사용합니다. 설정 키의 정확한 이름은 `is_analyzer`이며 `is_anlayzer`는 허용하지 않습니다. 잘못된 자료형, null, 필수 키 누락, 중복 키, 알 수 없는 키, 지원하지 않는 스키마 버전은 거부합니다. 설정은 최대 64KiB의 일반 파일이어야 하며 심볼릭 링크와 FIFO는 허용하지 않습니다. 새로운 동작 설정은 이름·자료형·검증 규칙을 정의해 추가합니다.

데몬은 수집 주기와 별도로 1초마다 설정 파일 전체를 확인하므로 재시작 없이 모든 항목을 자동 반영합니다. `is_analyzer`가 `true`인 경우에만 계정 파일을 확인하고 DB 연결을 준비합니다. 파일을 직접 수정하는 방식과 새 파일로 원자 교체하는 방식 모두 지원합니다. 진행 중인 동기 수집·파일 작업이 오래 걸리면 확인도 그 작업이 끝난 뒤 수행됩니다. 명시한 `--interval-seconds`와 `--output` 옵션은 실행 중에도 각각 JSON의 값을 덮어씁니다. 기본 systemd 서비스는 이 옵션들을 지정하지 않아 파일 설정을 사용합니다.

수집 주기가 바뀌면 적용한 시점부터 새 주기만큼 기다린 뒤 다음 JSON을 저장합니다. 예를 들어 60초에서 5초로 변경하면 다음 설정 확인 후 약 5초 뒤 수집합니다. CPU 기준 샘플을 버리지 않으므로 전환 직후 `cpu.window_seconds`는 직전 샘플부터의 실제 관측 시간이며 새 주기와 다를 수 있습니다. 저장 경로나 임계치만 바뀌거나 문서 내용이 동일하면 수집 타이머를 다시 시작하지 않습니다.

저장 경로를 바꾸려면 먼저 로컬 디렉터리를 만들고 `resource-monitor` 계정에 쓰기·탐색 권한을 부여해야 합니다. 데몬은 임의의 새 경로를 자동 생성하지 않습니다. 별도 마운트에 저장한다면 `systemctl edit resource-monitor`의 `[Unit]`에 `RequiresMountsFor=/새/저장/경로`도 추가하고 SELinux 접근을 확인하십시오. 기본 단위 파일의 마운트 의존성은 `/Monitor`입니다.

사용률이 임계치 이상이면 `warning`, 미만이면 `ok`, 측정 불가면 `unknown`입니다. 표시를 위해 값을 먼저 반올림하지 않습니다. 스토리지는 파일시스템 각각에 같은 임계치를 적용합니다. 임계치 아래로 내려오면 다음 측정부터 즉시 `ok`로 돌아옵니다.

설정 파일 검증과 새 저장 경로 준비에 성공하면 수집 주기·경로·임계치·보존 정책·장비 역할 등 일반 설정을 적용합니다. DB 연결 준비나 재연결이 실패해도 유효한 일반 설정 변경은 반영합니다. DB 연결은 각 조회 채널에서 후보 연결 검증이 끝난 뒤 따로 전환합니다. 새 저장 경로는 디렉터리 잠금을 확보하고 실제 임시파일 쓰기·동기화·정리를 확인한 뒤 전환합니다. 기존 경로의 잠금은 전환 완료 시 해제합니다. 같은 디렉터리를 가리키는 경로 별칭은 기존 잠금을 재사용합니다.

문서 오류, 없는 경로, 권한 부족, 다른 데몬의 잠금 등으로 일반 설정 변경을 적용하지 못하면 기존 유효 설정으로 수집을 계속하고 JSON에 `config_status=using_last_valid`와 오류를 기록합니다. 실패 원인은 journal에도 남깁니다. 매초 다시 시도하므로 디렉터리를 준비하거나 잠금을 해제하면 파일을 다시 수정하지 않아도 자동 반영합니다. `config_status`는 일반 설정의 유효성과 적용 상태이며 DB 접속 성공 여부를 뜻하지 않습니다. 시작부터 설정 문서가 잘못된 경우 종료 코드 78로 실패하며 systemd는 이 코드로 반복 재시작하지 않습니다. `--check-config`는 문서 형식과 `is_analyzer`가 `true`일 때 계정 파일을 검증합니다. DB 네트워크 접속과 실제 저장 경로 준비는 실행 시 검증합니다.

사용자가 수정한 파일을 데몬이 덮어쓰지 않습니다. 편집 중간의 일시적 파싱 실패를 줄이려면 같은 디렉터리에 새 파일을 작성·검증한 뒤 원자적으로 교체하십시오. 별도 검증 명령은 다음과 같습니다.

```bash
resource-monitor --config /etc/resource-monitor/settings.json --check-config
```

## MySQL 연결 설정

MySQL 연결과 쿼리는 최상위 `is_analyzer`가 `true`일 때만 실행합니다. `is_analyzer`는 생략하면 `false`이며 문자열 `"true"`, 숫자 `1`, `null`은 허용하지 않습니다. JSON에는 소문자 `true` 또는 `false`를 입력하십시오. `is_analyzer`가 `true`이면 `mysql` 객체와 `mysql.database`를 반드시 지정해야 합니다. `settings.json`의 `mysql`에는 접속 대상과 동작 설정만 저장하고 `username` 또는 `password`를 직접 넣으면 설정을 거부합니다.

1.3.3부터 `mysql.enabled`를 제거하고 `is_analyzer` 하나로 DB 사용 여부를 제어합니다. 업그레이드 전에 기존 `settings.json`의 `mysql.enabled`를 삭제하고 원하는 동작에 맞게 최상위 `is_analyzer`를 지정하십시오. 제거된 키가 남아 있으면 알 수 없는 키로 설정을 거부하며, RPM 업그레이드는 기존 사용자 설정을 자동으로 수정하지 않습니다. `is_analyzer`가 없거나 `false`이면 DB에 연결하거나 쿼리를 실행하지 않고 계정 파일도 읽지 않습니다. CPU·메모리·스토리지 수집, 수집 장비의 Java 프로세스 모니터링과 JSON 저장은 계속합니다.

| 설정 | 의미와 기본값 |
| --- | --- |
| `mysql.host` | 접속할 호스트, 기본 `127.0.0.1` |
| `mysql.port` | TCP 포트, 1~65535, 기본 `3306` |
| `mysql.database` | 접속할 데이터베이스, `is_analyzer`가 `true`일 때 필수이며 배포 예제는 `monitoring` |
| `mysql.credentials_file` | 계정 파일의 절대 경로, 기본 `/etc/resource-monitor/secrets/mysql.json` |
| `mysql.connect_timeout_seconds` | 연결 제한 시간, 1~10초의 정수, 기본 3초 |
| `mysql.read_timeout_seconds` | 읽기 제한 시간, 1~10초의 정수, 기본 3초 |
| `mysql.write_timeout_seconds` | 쓰기 제한 시간, 1~10초의 정수, 기본 3초 |
| `mysql.tls_mode` | `verify_identity`, `disabled`, 기본 `verify_identity` |
| `mysql.tls_ca_file` | `is_analyzer`가 `true`인 `verify_identity` 연결에서 필수인 CA 인증서의 절대 경로 |

기본 `verify_identity`는 TLS 암호화와 서버 인증서·호스트명 검증을 모두 요구합니다. `tls_ca_file`에는 신뢰할 CA 인증서를 준비하고 인증서의 호스트명과 일치하는 `host`를 지정하십시오. IP 주소로 접속한다면 서버 인증서에 해당 IP의 SAN이 있어야 합니다. TLS를 제공하지 않는 로컬 테스트 DB를 사용할 때만 `disabled`를 명시하십시오. 인증서 검증 없이 TLS만 요구하는 모드는 제공하지 않습니다.

계정 파일의 형식은 다음과 같습니다. 아래 값은 예제이며 실제 DB에서 준비한 계정으로 바꾸어야 합니다.

```json
{
  "username": "replace-with-database-user",
  "password": "replace-with-database-password"
}
```

최초 한 번 다음 명령으로 파일을 준비하고 편집합니다. 기존 파일이 있으면 복사하지 않습니다.

```bash
sudo install -d -m 0750 -o root -g resource-monitor /etc/resource-monitor/secrets
if ! sudo test -e /etc/resource-monitor/secrets/mysql.json && ! sudo test -L /etc/resource-monitor/secrets/mysql.json; then
    sudo install -m 0640 -o root -g resource-monitor /usr/share/resource-monitor/mysql.example.json /etc/resource-monitor/secrets/mysql.json
fi
sudoedit /etc/resource-monitor/secrets/mysql.json
sudo chown root:resource-monitor /etc/resource-monitor/secrets/mysql.json
sudo chmod 0640 /etc/resource-monitor/secrets/mysql.json
sudo install -m 0640 -o root -g resource-monitor /path/to/trusted-ca.pem /etc/resource-monitor/secrets/mysql-ca.pem
sudoedit /etc/resource-monitor/settings.json
sudo -u resource-monitor /usr/sbin/resource-monitor --check-config
```

위 `/path/to/trusted-ca.pem`은 DB 관리자가 제공한 CA 인증서의 실제 경로로 바꾸십시오. `settings.json`에서 DB 주소·데이터베이스·TLS 설정을 맞추고 최상위 `is_analyzer`를 `true`로 변경하면 실행 중인 데몬이 연결을 준비합니다. `secrets/mysql.json`은 평문이므로 전용 서비스 계정과 root만 읽을 수 있도록 디렉터리와 파일 권한을 유지해야 합니다. 계정 파일은 root 또는 현재 실행 계정이 소유한 최대 16KiB의 일반 파일이어야 하며 권한은 0600 또는 0640만 허용합니다. 심볼릭 링크와 FIFO도 허용하지 않습니다. 사용자명은 제어문자가 없는 1~128바이트, 비밀번호는 NUL 문자가 없는 1~4096바이트 문자열입니다. 실제 파일과 `.env`는 Git 및 소스 패키지에서 제외하고, 배포본에는 예제 값이 있는 계정 템플릿만 포함합니다.

DB 사용이 허용된 동안 설정과 계정 파일을 1초마다 다시 읽으므로 비밀번호만 바꾸어도 재연결합니다. 1.11.0은 일별 건수·무수신·구간 수집량 조회에 각각 독립된 `MySqlConfiguration` 작업 스레드와 DB 세션을 사용합니다. 한 조회의 지연이나 연결 실패가 다른 조회의 연결을 막지 않으며, 리소스 수집도 DB 결과를 기다리지 않습니다. 세 조회는 DB 서버 자원을 공유하므로 서버 측 부하는 별도로 확인해야 합니다.

새 연결과 `SELECT 1` 확인에 성공한 채널부터 연결을 교체합니다. 후보 연결 검증 중에도 해당 채널의 기존 연결이 정상이면 계속 사용합니다. 주소·계정 오류, 접속 거부, TLS 오류가 발생해도 수집 주기·저장 경로·임계치·보존 정책 변경은 반영하며, 연결 실패는 5초부터 10·20·40초로 재시도 간격을 늘리고 최대 60초로 제한합니다. 연결 성공 또는 접속 설정 변경 시 지연을 초기화합니다. 대기 중 파일을 다시 수정하면 최신 설정에 맞는 연결만 채택합니다.

`is_analyzer`를 `false`로 바꾸거나 삭제하면 다음 설정 확인에서 기존 연결을 닫고 새 DB 연결·쿼리·계정 파일 읽기를 중단합니다. 데몬을 재시작할 필요가 없습니다. 이미 작업 스레드에 전달된 연결이나 쿼리는 완료될 수 있지만 비활성화 이전 작업의 결과는 채택하지 않습니다. 다시 `is_analyzer`를 `true`로 설정하면 최신 설정과 계정 파일로 연결을 준비합니다.

처음 실행할 때 DB에 연결할 수 없거나 계정 파일이 잘못되어도 리소스 수집은 유효한 수집 주기·저장 경로·임계치로 계속합니다. 첫 연결을 준비하는 동안에는 `pending`, 실패가 확인되면 해당 분석의 `status=error`와 오류를 기록합니다. 재시도를 시작했다는 이유만으로 기존 실패를 `connection_pending`으로 덮어쓰지 않습니다. DB 라이브러리가 제공한 숫자 오류 코드는 `error.code`에 보존하며 사용자명·비밀번호와 서버 원문 오류는 기록하지 않습니다. 연결과 조회가 복구되면 정상 결과를 다시 사용합니다. DB 오류는 CPU·메모리·스토리지 각각의 사용률과 상태를 변경하지 않으며, 분석을 완료하지 못하면 전체 `status`는 아래 JSON 계약에 따라 판정합니다.

DB 사용이 허용된 정상 연결은 30초마다 `SELECT 1`로 확인하며 끊어진 연결은 현재 설정으로 다시 준비합니다. 저장 경로 오류 등으로 적용을 기다리는 후보 연결도 30초가 지나면 새로 연결·검증합니다. 시간 제한은 연결 라이브러리의 네트워크 작업에 적용되며 운영체제의 DNS 조회 시간을 제한하지는 않습니다. 종료 시 진행 중인 DB 작업을 기다리고, systemd의 기존 `TimeoutStopSec=15`를 초과하면 systemd가 프로세스를 종료합니다.

DB에 실행하는 SQL은 연결 확인용 `SELECT 1`, 무수신 조회 인덱스 확인용 `SHOW INDEX`, 아래 일별 수집 건수·마지막 로그 수신 시각·구간 수집량 조회입니다. 테이블과 데이터를 생성·수정·삭제하지 않습니다. DB 계정에는 접속 데이터베이스에 대한 접근 권한과 `CloudESM.SUM_COLLECT_INFO_1DAY`, `CloudESM.SUM_COLLECT_INFO`의 `SELECT` 권한이 필요합니다. 조회 대상은 완전한 테이블명으로 고정되어 있으므로 `mysql.database`가 다른 데이터베이스여도 두 테이블을 조회합니다.

## 장비별 일별 수집 건수 분석

`is_analyzer=true`이면 `CloudESM.SUM_COLLECT_INFO_1DAY`의 `recv_time`, `eqp_ip`, `log_cate_cd`, `count`를 기준으로 분석합니다. 확인된 테이블에서 `recv_time`은 `20261019`처럼 `YYYYMMDD`를 저장하는 BIGINT, `eqp_ip`는 VARCHAR(39), `log_cate_cd`는 VARCHAR(20), `count`는 NULL을 허용하는 signed BIGINT입니다. 기본 키는 `(recv_time, eqp_ip, log_cate_cd)`입니다. 같은 날짜와 IP의 모든 로그 분류를 합산하여 비교하며 `length` 열은 사용하지 않습니다.

기본 `analyzer.comparison_day="yesterday"`는 어제와 그제를 비교합니다. 하루가 끝난 날짜끼리 비교하므로 오늘의 부분 누적 건수를 어제 전체와 비교하면서 생길 수 있는 경고를 피할 수 있습니다. 예를 들어 OS 날짜가 2026-10-19이면 `current_day=20261018`, `previous_day=20261017`입니다. `today`를 지정하면 각각 `20261019`, `20261018`이며 오늘은 조회 시점까지 누적된 값입니다. 두 방식 모두 서버의 OS 시간대를 따르고 DB 세션의 `CURDATE()`를 사용하지 않습니다. 날짜 변경 시 새 날짜 쌍을 조회합니다. 일별 테이블 자체의 집계·적재가 늦게 완료되는 경우 해당 DB에 현재 저장된 값을 사용합니다.

다음은 기본 날짜 쌍에 대해 실행하는 SQL입니다. `recv_time`을 함수로 감싸지 않고 날짜 값으로 제한하므로 날짜가 선두인 기본 키를 사용할 수 있습니다. 유효하고 인접한 두 달력 날짜만 숫자로 구성하며 사용자 지정 SQL이나 테이블명을 받지 않습니다.

```sql
SELECT eqp_ip,
       SUM(CASE WHEN recv_time = 20261018 THEN `count` ELSE 0 END),
       SUM(CASE WHEN recv_time = 20261017 THEN `count` ELSE 0 END),
       COUNT(CASE WHEN recv_time = 20261018 THEN 1 END),
       COUNT(CASE WHEN recv_time = 20261017 THEN 1 END),
       COUNT(CASE WHEN recv_time = 20261018 THEN `count` END),
       COUNT(CASE WHEN recv_time = 20261017 THEN `count` END),
       MIN(CASE WHEN recv_time = 20261018 THEN `count` END),
       MIN(CASE WHEN recv_time = 20261017 THEN `count` END)
FROM CloudESM.SUM_COLLECT_INFO_1DAY
WHERE recv_time IN (20261017, 20261018)
GROUP BY eqp_ip
ORDER BY eqp_ip
LIMIT 10001;
```

`previous_count`가 0보다 크면 `(current_count - previous_count) / previous_count * 100`으로 증감률을 계산합니다. 절댓값이 `analyzer.collect_count_change_percent` 이상이면 `alerts`에 기록합니다. 임계치 10일 때 정확히 +10%와 -10%도 경고이며, 판정 전에 비율을 반올림하지 않습니다. 증가의 `reason`은 `increase`, 감소는 `decrease`입니다.

두 날짜 중 어느 쪽에라도 존재하는 IP를 비교 대상에 포함합니다. 한 날짜의 행이 없으면 그 날짜의 건수는 0이며 `current_present` 또는 `previous_present`를 `false`로 표시합니다. 기준일 건수가 0이고 비교일 건수가 양수이면 비율을 정의할 수 없으므로 `change_percent=null`, `reason=baseline_zero`로 경고합니다. 양쪽 건수가 0이면 정상입니다. 두 날짜 모두에 행이 없는 IP는 발견할 수 없으며 전체 조회 결과가 비어 있으면 정상으로 간주하지 않고 `no_data`를 기록합니다.

실제 행의 `count`가 NULL 또는 음수이거나, 합계가 unsigned 64비트 정수 범위를 넘거나, 장비 IP 값이 유효하지 않으면 해당 조회 전체를 `error`로 기록합니다. 일부 항목만 정상인 것처럼 저장하지 않습니다. 최대 10,000개 IP를 처리하며 10,001번째 그룹이 있으면 오류를 기록합니다. SQL 결과는 스트리밍으로 읽습니다. 조회 오류가 있어도 리소스 수집과 파일 저장은 계속하며 DB 원문 오류나 계정 정보를 기록하지 않습니다.

조회는 DB 작업 스레드에서 수행하므로 리소스 수집이 DB 결과를 기다리지 않습니다. 시작이나 날짜 전환 직후에는 `pending`일 수 있습니다. 완료된 결과를 캐시에 보관하고 완료 시점부터 `collection.interval_seconds` 간격으로 갱신합니다. JSON의 `queried_at`은 해당 조회가 완료된 시각이며 스냅샷의 `timestamp`와 다를 수 있습니다. 결과가 수집 주기의 두 배 또는 10초 중 더 긴 시간을 초과하여 오래되면 `pending`으로 표시합니다. 조회 오류는 수집 주기와 5초 중 더 긴 간격으로 재시도합니다. 분석 임계치 변경은 다음 분석부터 캐시 결과에 적용하고 DB에 재연결하지 않습니다. `is_analyzer=false`이면 분석은 `disabled`이고 DB 연결·조회도 중단합니다.

다음은 전일 대비 10% 증가한 장비가 포함된 JSON의 `collect_count` 부분입니다. `alerts`에는 경고 대상만 기록하고 `equipment_count`에는 전체 비교 대상 IP 수를 기록합니다.

```json
{
  "collect_count": {
    "source": "CloudESM.SUM_COLLECT_INFO_1DAY",
    "status": "warning",
    "current_day": 20261018,
    "previous_day": 20261017,
    "queried_at": "2026-10-19T12:33:59.123+09:00",
    "equipment_count": 2,
    "alerts": [
      {
        "eqp_ip": "192.0.2.10",
        "current_count": 110,
        "previous_count": 100,
        "current_present": true,
        "previous_present": true,
        "change_percent": 10.0,
        "reason": "increase"
      }
    ],
    "error": null
  }
}
```

## 장비별 시간 구간 로그 수집량

1.11.0의 `log_volume`은 선택한 기간 안의 `count`를 `eqp_ip`별로 합산합니다. 같은 장비의 모든 `log_cate_cd`를 함께 합산하며, 기존 `collect_count`의 일별 증감 경고와 `log_silence`의 무수신 판정은 각각 유지합니다. `is_analyzer=false`이면 `log_volume.status=disabled`이고 이 기능도 DB에 접속하거나 조회하지 않습니다.

5분마다 최근 완료된 5분을 집계하는 설정입니다.

```json
"log_volume": {
  "mode": "interval",
  "interval_seconds": 300,
  "daily_at": "00:10"
}
```

`interval`은 Unix epoch를 기준으로 길이가 고정된 완료 구간을 선택합니다. 5분 설정에서 현지 시각 10:05에 `[10:00, 10:05)`를, 10:10에 `[10:05, 10:10)`를 조회합니다. 시작은 포함하고 끝은 제외하므로 경계의 같은 분을 두 구간에 중복 합산하지 않습니다. DB의 `recv_time`은 분 단위 `YYYYMMDDHHmm`이며 SQL은 다음 형태입니다.

```sql
SELECT eqp_ip, SUM(`count`)
FROM CloudESM.SUM_COLLECT_INFO
WHERE recv_time >= 202610291000
  AND recv_time < 202610291005
GROUP BY eqp_ip
ORDER BY eqp_ip
LIMIT 10001;
```

매일 현지 시각 00:10에 전날 하루를 집계하려면 다음처럼 지정합니다.

```json
"log_volume": {
  "mode": "daily",
  "interval_seconds": 300,
  "daily_at": "00:10"
}
```

`daily`에서는 `interval_seconds`를 예약에 사용하지 않습니다. 10월 29일 00:10에는 10월 28일 00:00부터 10월 29일 00:00 직전까지를 의미하는 일별 요약을 조회합니다. `SUM_COLLECT_INFO_1DAY.recv_time`은 `YYYYMMDD`이므로 해당 SQL 범위는 `recv_time >= 20261028 AND recv_time < 20261029`입니다. 이 테이블에 저장된 일별 `count`를 장비별로 합산하며 원본 로그 한 건마다 나노초 시각으로 다시 구분하는 방식은 아닙니다.

예약은 `collection.interval_seconds`와 독립적입니다. 실행 시 가장 최근 예약을 지난 완료 구간 하나를 즉시 요청합니다. 예를 들어 00:10 예약인데 00:09에 시작했다면 마지막으로 예약을 지난 그저께 구간을 요청하고, 00:10에 새로 어제 구간을 요청합니다. 중지 기간의 모든 과거 구간을 순서대로 복구하지는 않습니다. 실행 중 모드·간격·예약 시각을 변경하면 다음 설정 확인부터 새 기준의 최근 완료 구간을 선택합니다.

완료 여부는 약 1초마다 확인합니다. 새 조회 결과나 오류가 생기면 정기 리소스 수집 시각 외에도 전체 모니터링 JSON을 추가 저장합니다. 따라서 일반 수집 주기가 1시간이어도 일일 예약 시각에 조회를 요청하고, DB 조회 완료 후 다음 확인 시 결과를 저장합니다. 동기 파일 작업이나 DB 응답이 지연되면 실제 저장도 늦어질 수 있습니다. `--once`는 추가 저장 없이 지정된 리소스 관측 구간 후 한 파일만 저장합니다.

결과 예시는 다음과 같습니다. 실제 파일에는 다른 모니터링 항목도 함께 있습니다.

```json
"log_volume": {
  "source": "CloudESM.SUM_COLLECT_INFO",
  "status": "ok",
  "mode": "interval",
  "window_start": "2026-10-29T10:00:00.000+09:00",
  "window_end_exclusive": "2026-10-29T10:05:00.000+09:00",
  "queried_at": "2026-10-29T10:05:00.250+09:00",
  "equipment_count": 2,
  "equipment": [
    {"eqp_ip": "192.0.2.10", "count": 12500},
    {"eqp_ip": "192.0.2.11", "count": 0}
  ],
  "error": null
}
```

한 구간의 조회가 성공하면 다음 구간까지 재조회하지 않고 캐시 결과를 사용합니다. 같은 결과가 이후 정기 JSON에도 반복 표시될 수 있으므로 소비자는 호스트·`mode`·`window_start`·`window_end_exclusive`를 식별 기준으로 삼아 같은 구간을 중복 합산하지 않아야 합니다. `queried_at`은 실제 SQL 완료 시각이며 JSON 저장 시각으로 갱신하지 않습니다. 새 구간의 조회가 끝나기 전에는 `pending`으로 표시하고 이전 구간의 건수를 새 기간의 값처럼 사용하지 않습니다.

기간에 행이 없는 IP는 `equipment`에 포함하지 않습니다. 기간 전체에 행이 없으면 `status=no_data`, `equipment=[]`이며 모든 장비의 수집량이 0이라는 뜻은 아닙니다. 행이 있고 정상적인 `count` 합계가 0이면 그 장비는 `count: 0`으로 표시합니다. 실제 SQL에는 `COUNT(*)`, `COUNT(count)`, `MIN(count)` 검증도 포함합니다. NULL·음수·합계 범위 초과·잘못된 장비 식별 값은 조회 전체의 `error`로 처리합니다. 최대 10,000개 장비를 지원하며 `LIMIT 10001`로 초과 여부를 확인하고 결과를 조용히 잘라내지 않습니다.

조회 실패는 기존 DB 재시도 정책을 적용하며 오류 중에는 과거 건수를 정상 값으로 표시하지 않습니다. 성공한 기간에 늦게 데이터가 적재되거나 기존 행이 수정되어도 자동으로 재집계하지 않습니다. 최신 완료 구간의 정정이 필요하면 데이터 적재를 확인한 뒤 데몬을 재시작해 그 구간을 다시 조회하십시오. 이전 구간까지 조회할 필요가 있다면 별도 SQL 조회가 필요합니다. 재시작으로 얻은 같은 기간의 정정값도 소비 시스템에서는 기존 값의 교체로 처리해야 합니다.

데몬 OS의 시간대와 DB `recv_time` 값이 뜻하는 현지 시간대가 일치해야 합니다. `interval`은 분 단위 자료를, `daily`는 일별 요약을 조회합니다. 일광 절약 시간 전환일의 일일 경계는 현지 자정으로 계산하므로 실제 경과 시간은 23시간 또는 25시간일 수 있습니다. 반복되는 현지 분 시각 때문에 interval 경계를 유일하게 표현할 수 없으면 `ambiguous_interval` 오류로 표시합니다. 이 기능은 조회만 수행하며 DB 테이블·트리거·인덱스를 생성하거나 변경하지 않습니다.

## Java 프로세스 모니터링

`is_analyzer=true`이면 `process_monitor.analyzer`, `false`이면 `process_monitor.collector` 목록을 선택하여 매 수집 주기에 Java 프로세스를 관측합니다. 대상은 `-DAPP=<이름>`을 기준으로 하며 대소문자를 구분합니다. 다음은 각 목록을 생략했을 때 사용하는 기본값입니다.

| `is_analyzer` | `process_profile` | 대상 DAPP 이름 |
| --- | --- | --- |
| `true` | `analyzer` | `SearchEngine`, `Normalizer`, `CollectorForAgent`, `CDMEngine`, `ShovelServer`, `AnalyzeEngine`, `AnalyzeAssistantEngine`, `SoarTicketMain`, `SoarPlaybookManager`, `pmsServer`, `pmsClient`, `ProcessMonitor` |
| `false` | `collector` | `SearchEngine`, `Reducer`, `Normalizer`, `CloudESMCollector`, `CollectorForAgent`, `CDMEngine`, `pmsClient`, `ProcessMonitor` |

두 목록은 각각 0~256개의 문자열을 허용합니다. 이름은 UTF-8 기준 1~128바이트이며 제어문자와 앞뒤 ASCII 공백을 허용하지 않습니다. 이름 내부의 공백은 허용하며 JVM 인자 값 전체와 정확히 비교합니다. 같은 목록에서 동일한 이름을 중복 지정할 수 없으며 대소문자가 다른 이름은 서로 다른 대상입니다. 현재 선택되지 않은 역할의 목록도 함께 검증합니다. `process_monitor` 객체 전체를 생략하거나 한쪽 목록만 생략하면 해당 목록에 기존 기본값을 사용합니다. 빈 배열 `[]`을 명시하면 해당 역할의 프로세스 관측을 생략하고 `processes={}`, `process_status=ok`를 기록합니다. 이 경우에도 리소스 수집과 `is_analyzer`에 따른 DB 동작은 계속합니다.

`/proc`에서 Java 프로세스의 인자를 확인하며 `ps`나 `grep` 명령을 실행하지 않습니다. `-DAPP`가 없는 Tomcat·메신저 등의 Java 프로세스와 `grep java`는 대상에 포함하지 않습니다. 이름의 일부가 일치하는 것만으로는 실행 중으로 판정하지 않습니다. 같은 DAPP 이름으로 여러 프로세스가 실행 중이면 살아 있는 인스턴스가 하나라도 있을 때 `true`입니다.

`processes`는 선택한 대상 이름을 모두 포함합니다. 실행 중인 대상은 `true`, 관측을 완료했으나 해당 프로세스가 없거나 종료·좀비 상태이면 `false`입니다. 실행 여부를 확인하는 기능이므로 `true`가 애플리케이션의 응답 정상이나 처리 정상까지 보장하지는 않습니다. 대상의 미실행을 확인하면 `process_status=warning`이며 전체 `status`에도 경고를 반영합니다. 누락된 프로세스를 자동으로 시작하거나 재시작하지 않습니다.

프로세스 목록이나 인자를 권한 문제 등으로 충분히 확인하지 못하면 발견된 실행 중 대상은 `true`로 유지하고, 존재 여부를 확정할 수 없는 대상은 `null`로 기록합니다. 이 경우 `process_status=unknown`과 `errors`에 관측 오류를 남깁니다. 서비스 계정이 호스트의 프로세스를 볼 수 있도록 `/proc` 접근 권한을 준비해야 합니다. 프로세스 조회도 순차적으로 수행하므로 저장 순간과 완전히 동일한 시점의 원자적 상태는 아닙니다.

다음은 수집 장비에서 `Reducer`가 미실행인 경우의 JSON 일부입니다. 값은 문자열이 아닌 JSON 불리언입니다.

```json
{
  "status": "warning",
  "process_profile": "collector",
  "process_status": "warning",
  "processes": {
    "SearchEngine": true,
    "Reducer": false,
    "Normalizer": true,
    "CloudESMCollector": true,
    "CollectorForAgent": true,
    "CDMEngine": true,
    "pmsClient": true,
    "ProcessMonitor": true
  }
}
```

`settings.json`의 `is_analyzer` 또는 `process_monitor` 목록을 수정하면 1초마다 수행하는 설정 확인을 거쳐 재시작 없이 다음 수집부터 적용합니다. 목록에 추가한 이름은 다음 결과에 나타나고 삭제한 이름은 결과에서 사라집니다. 목록만 변경하면 DB를 재연결하지 않습니다. 중복 이름이나 문자열이 아닌 항목 등으로 적용에 실패하면 기존 설정 전체와 대상 목록을 유지하고 `config_status=using_last_valid`를 기록합니다. 올바른 파일로 수정하면 자동으로 복구합니다. 프로세스 관측은 DB 쿼리 결과에 의존하지 않으며 DB 분석 중단이나 조회 실패 중에도 계속합니다. 수집 주기와 저장 경로는 기존 `collection` 설정을 함께 사용합니다.

## 장비별 로그 무수신 모니터링

실제 대상 테이블은 `CloudESM.SUM_COLLECT_INFO`입니다. `SUM_COLLCECT_INFO`는 테이블명 오기입니다. 확인한 주요 컬럼은 `eqp_ip varchar(39)`, `recv_time bigint`, `reg_time bigint`, `count int`이며, `recv_time`은 `YYYYMMDDHHmm` 형식의 분 단위 로그 수집 시각입니다. `reg_time`은 수집 현황 등록 시각이므로 마지막 로그 수신 판정에 사용하지 않습니다.

`is_analyzer=true`일 때 수집 주기에 맞춰 무수신 전용 DB 작업 스레드가 장비별 마지막 수신 분을 조회합니다. 각 장비의 `count > 0`인 행 중 가장 큰 `recv_time`을 사용합니다. 장비 IP는 매번 DB의 전체 `eqp_ip`에서 발견하며 소스 코드나 설정 파일의 목록으로 제한하지 않습니다. 새 IP는 다음 조회부터 자동으로 나타나고 테이블에서 모든 행이 제거된 IP는 다음 조회부터 결과에서 사라집니다. NULL·빈 `eqp_ip`는 장비 식별이 불가능하므로 제외합니다. 최대 10,000개 장비를 지원하며 한도를 넘기면 결과를 자르는 대신 오류를 기록합니다.

기존 쿼리도 DB 서버에서 `GROUP BY eqp_ip`와 `MAX(CASE WHEN count > 0 THEN recv_time END)`를 계산하여 장비별 결과만 반환합니다. 1.10.0은 사용할 수 있는 인덱스를 확인하면 `DISTINCT eqp_ip`로 모든 장비를 발견하고, 장비별로 `count > 0` 조건에 맞는 최신 행을 `ORDER BY recv_time DESC LIMIT 1`로 조회합니다. 데몬에 전달되는 값은 장비 IP, 마지막 수신 분, 그 시각의 epoch와 조회 기준 DB 시각 네 열입니다. 원본 로그 행 전체를 클라이언트로 가져오거나 전체 수집 건수를 `COUNT`하지 않습니다.

인덱스 확인은 각 DB 세션에서 `SHOW INDEX`로 한 번 수행합니다. 사용 조건은 선두 열이 `(eqp_ip, recv_time, count)` 순서이고, 접두 길이로 잘리지 않은 세 열을 포함하는 사용 가능한 BTREE 인덱스입니다. 무시 또는 비가시 상태인 인덱스는 선택하지 않습니다. 조건에 맞으면 다른 이름의 기존 인덱스도 사용할 수 있습니다. 없으면 기존의 DB 집계 쿼리를 사용하여 인덱스 없는 장비별 반복 검색을 피합니다. 인덱스를 추가·변경한 뒤에는 데몬을 재시작하거나 해당 DB 연결을 새로 맺어야 선택이 갱신됩니다.

오랫동안 수신하지 않은 장비도 발견해야 하므로 최근 시각 조건으로 조회 범위를 잘라내지 않습니다. 양수 `count`가 없는 장비도 결과에 남으며 마지막 수신 시각은 `null`입니다. 그런 장비에 0건 기록이 많이 쌓였거나 최근 기록 대부분이 0건이면 해당 장비의 인덱스 범위를 여러 행 검사할 수 있습니다. 실제 조회의 `EXPLAIN`, 테이블 크기·인덱스와 실행 시간을 확인해 효과를 판단하십시오. 연결 실패 로그만으로 집계 시간 초과를 원인으로 확정하지 말고 보존된 오류 코드와 DB 서버 상태를 함께 확인해야 합니다.

### 무수신 조회 인덱스 준비

RPM은 DBA가 선택해 실행할 SQL을 `/usr/share/resource-monitor/sql/log_silence_index.sql`에 설치합니다. 소스 파일은 `packaging/sql/log_silence_index.sql`입니다. 데몬과 RPM 설치 스크립트는 이 DDL을 실행하지 않으며 데몬 계정은 두 모니터링 테이블의 `SELECT` 권한을 유지합니다.

DBA는 먼저 `SHOW INDEX`에서 같은 역할의 인덱스가 이미 있는지 확인합니다. `Seq_in_index` 1·2·3이 각각 `eqp_ip`, `recv_time`, `count`이고 `Sub_part`가 모두 NULL인 BTREE 인덱스가 사용 가능한 상태라면 별도 인덱스를 중복 생성할 필요가 없습니다.

```sql
SHOW INDEX FROM CloudESM.SUM_COLLECT_INFO;
```

필요한 경우 제공 SQL은 다음 인덱스를 생성합니다.

```sql
CREATE INDEX IF NOT EXISTS ix_resource_monitor_eqp_recv_count
ON CloudESM.SUM_COLLECT_INFO (eqp_ip, recv_time, `count`)
ALGORITHM=INPLACE LOCK=NONE;
```

이 작업은 해당 테이블의 `INDEX` 권한이 있는 DBA 계정으로 수행합니다. `IF NOT EXISTS`는 인덱스 이름의 존재만 확인하므로 같은 이름에 다른 열 정의가 있어도 수정하지 않습니다. 실행 전후 `SHOW INDEX`로 실제 정의를 확인하십시오. 온라인 DDL 옵션을 지정해도 메타데이터 잠금 대기가 발생할 수 있으며 엔진·버전에 따라 지원 여부를 확인해야 합니다. [MariaDB CREATE INDEX 문서](https://mariadb.com/docs/server/reference/sql-statements/data-definition/create/create-index)

DB 서버에서 로컬 소켓으로 관리자 접속이 가능한 경우, Linux 셸에서 다음처럼 실행합니다. 비밀번호는 프롬프트에서 입력합니다. 원격 DB를 관리하는 경우에는 관리자가 사용하는 호스트·포트·계정 옵션을 추가합니다.

```bash
mariadb -u root -p CloudESM -e 'SHOW INDEX FROM CloudESM.SUM_COLLECT_INFO;'
mariadb -u root -p CloudESM < /usr/share/resource-monitor/sql/log_silence_index.sql
systemctl restart resource-monitor
```

DB 서버와 데몬 서버가 다르면 SQL은 DB에 적용하고 서비스 재시작은 데몬 서버에서 수행합니다. 테이블 원본 데이터의 생성·수정·삭제는 수행하지 않습니다.

```json
"log_silence": {
  "default_timeout_seconds": 300,
  "overrides": []
}
```

위 설정은 DB에서 발견한 모든 장비에 5분을 적용합니다. 기본값이나 예외 목록을 생략해도 같은 기본값을 사용합니다. 장비마다 다른 시간이 필요한 경우에만 `overrides`에 해당 `eqp_ip`와 `timeout_seconds`를 넣습니다. 예외 설정은 조회 범위를 제한하거나 DB에 없는 장비를 생성하지 않습니다. 빈 예외 목록은 모든 장비에 기본 시간을 적용한다는 뜻이며 감시 비활성화가 아닙니다. 설정은 1초마다 다시 읽고 다음 수집부터 적용하며 DB를 재연결하지 않습니다. 시간은 정수 초 단위 60~604800이고, 잘못된 자료형·중복 IP·유효하지 않은 IP·알 수 없는 키는 기존 설정 전체를 유지합니다.

DB에는 초 단위 수신 시각이 없으므로 해당 분의 끝을 기준으로 무수신 시간을 보수적으로 계산합니다. `silent_for_seconds = max(0, checked_at - last_received_at - 60초)`이며 이 값이 제한 시간 이상이면 경고합니다. 예를 들어 마지막 수신 분이 14:00, 제한이 300초라면 14:06부터 시간 초과 판정이 가능합니다. 실제 JSON 반영 시점에는 수집 주기와 DB 조회 시간이 추가될 수 있습니다.

같은 JSON 파일의 `log_silence`에 모든 발견 장비의 상태를 기록합니다. 다음은 설명용 경고 예시이며 예시 IP를 감시 대상으로 등록하지 않습니다.

```json
"log_silence": {
  "source": "CloudESM.SUM_COLLECT_INFO",
  "status": "warning",
  "last_received_precision_seconds": 60,
  "checked_at": "2026-10-19T14:07:00.000+09:00",
  "queried_at": "2026-10-19T14:07:00.020+09:00",
  "equipment_count": 1,
  "equipment": [
    {
      "eqp_ip": "192.0.2.10",
      "last_received_at": "2026-10-19T14:00:00.000+09:00",
      "silent_for_seconds": 360,
      "status": "warning",
      "reason": "timeout_exceeded"
    }
  ],
  "error": null
}
```

`checked_at`은 조회 당시 DB 시각이고 `queried_at`은 데몬이 조회를 완료한 시각입니다. 저장 시각으로 오래된 조회 결과를 계속 늘려 오경보를 만들지 않도록 판정은 `checked_at`을 기준으로 합니다. DB 수신 시각은 DB 세션의 시간대에서 epoch로 변환하고 JSON은 기존처럼 OS 시간대와 UTC 오프셋으로 출력합니다. 허용 수명보다 오래된 캐시는 `pending`으로 처리합니다.

장비별 `reason`은 정상 `receiving`, 시간 초과 `timeout_exceeded`, 양수 수집 기록 없음 `no_received_data`, 미래 수신 시각 `future_received_time`입니다. 양수 수집 기록이 없으면 마지막 시각과 무수신 시간은 `null`이며 경고로 기록합니다. 미래 수신 시각은 정상으로 간주하지 않고 `unknown`으로 표시합니다. 빈 테이블은 `no_data`, DB 접속·조회 실패는 `error`, 최초 조회 준비 중에는 `pending`, 수집 서버(`is_analyzer=false`)에서는 `disabled`입니다. DB 실패 시 장비 목록을 새 무수신 경고로 만들거나 과거 결과의 무수신 시간을 계속 늘리지 않습니다. `log_silence.error`와 최상위 `errors`에서 원인을 확인하며, 재시도 중에도 확인된 오류가 유지됩니다.

무수신 경고는 최상위 `status=warning`에 반영합니다. `pending`, `no_data`, `error`는 다른 경고가 없으면 최상위 `unknown`에 반영하며 리소스 전용 `collection_status`에는 영향을 주지 않습니다. DB 오류 중에도 CPU·메모리·디스크·DAPP 수집과 JSON 저장은 계속합니다. 이 기능은 로그 수신을 관측하며 장비 프로세스를 중지·시작하거나 DB 원본 데이터를 변경하지 않습니다.

## 수집 장비 처리 파일 개수 모니터링

`is_analyzer=false`일 때만 `/CloudESM/data/processing/collector`와 `/CloudESM/data/processing/normalize`를 하위 폴더까지 검사합니다. 각 루트 폴더에서 일반 파일의 `.tmp`와 `.done` 개수를 따로 합산하므로 총 네 개의 임계치를 적용합니다. 하위 폴더별 목록이나 파일 내용은 기록하지 않습니다. 대소문자를 구분하며 숨김 파일도 포함합니다. 이름이 `.tmp`로 끝나더라도 디렉터리·FIFO·심볼릭 링크는 세지 않습니다. 하위 심볼릭 링크 디렉터리도 따라가지 않습니다. `/CloudESM/data` 같은 상위 경로의 링크는 허용하지만 검사 대상 루트인 `collector`·`normalize` 자체가 링크이면 오류로 처리합니다.

기본 임계치는 모두 100개입니다. 예를 들어 collector의 `.tmp`가 99개면 해당 항목을 생략하고, 100개부터 출력합니다. 한 항목이라도 임계치 이상이면 `processing_files.alerts`에 그 항목만 추가하고 최상위 `status`를 `warning`으로 반영합니다. 네 항목 모두 임계치 미만이거나 분석 서버이면 `processing_files` 객체 자체를 생략합니다. CPU·메모리·디스크 등 기존 결과 파일은 매 수집 주기마다 계속 생성됩니다. 임계치 0은 명시적으로 허용하며, 정상 검사한 개수가 0이어도 해당 항목을 기록합니다.

```json
"processing_files": {
  "status": "warning",
  "scan_interval_seconds": 60,
  "scan_started_at": "2026-10-19T15:00:00.000+09:00",
  "scanned_at": "2026-10-19T15:00:00.125+09:00",
  "alerts": [
    {
      "path": "/CloudESM/data/processing/collector",
      "extension": ".tmp",
      "count": 100
    }
  ]
}
```

파일 검사는 별도 작업 스레드에서 시작 직후 한 번, 이후 단조 시계 기준 60초마다 수행합니다. `collection.interval_seconds`를 변경해도 검사 주기는 60초로 유지합니다. 진행 중인 검사를 겹쳐 실행하지 않으며 오래 걸려 지나간 검사 시점은 건너뜁니다. JSON 저장 시 가장 최근 완료된 검사 결과를 사용하고, `scan_started_at`·`scanned_at`으로 실제 검사 구간을 표시합니다. 저장 주기가 짧으면 같은 검사 결과가 여러 JSON에 사용될 수 있고, 저장 주기가 길면 그 사이의 모든 검사 결과를 별도 파일로 남기지는 않습니다. 폴더가 검사 중 변경될 수 있으므로 동시에 고정된 파일시스템 스냅샷은 아닙니다.

설정 파일의 임계치 변경은 재시작 없이 기존의 유효한 검사 결과에 적용합니다. 역할을 분석 서버로 바꾸면 진행 중인 검사에 취소를 요청하고 이전 결과를 버리며, 수집 서버로 돌아오면 새 검사를 시작합니다. 검사 준비 중이거나 완료 후 120초 넘게 오래된 결과는 경고 개수로 재사용하지 않습니다. 파일 작업이 커널에서 지연되면 취소 처리는 해당 호출이 반환된 뒤 진행됩니다.

없는 폴더, 접근 권한 오류, 검사 제한 초과는 0개로 처리하지 않습니다. 오류가 발생한 루트의 개수는 경고 판정에 사용하지 않고 최상위 `errors`에 `component=processing_files`로 원인을 기록합니다. 다른 루트를 정상 검사했다면 그 결과는 사용합니다. 오류 외에 경고가 없으면 최상위 상태는 `unknown`이며 리소스 전용 `collection_status`에는 영향을 주지 않습니다. 과도한 탐색을 막기 위해 하위 깊이 128, 루트당 1천만 항목, 전체 검사 시간 55초의 제한을 적용합니다. 데몬은 감시 폴더나 파일을 생성·수정·삭제하지 않습니다. 서비스 계정에 대상 폴더와 하위 폴더의 읽기·탐색 권한을 제공해야 합니다.

## JSON 계약

| 필드 | 값과 의미 |
| --- | --- |
| `schema_version` | 1 |
| `timestamp` | OS 시간대와 UTC 오프셋을 포함한 ISO 8601 수집 시각, 예: `2026-10-19T12:34:00+09:00` |
| `thresholds_applied_at` | 현재 임계치가 적용된 시각을 OS 시간대로 표시한 값, 밀리초와 UTC 오프셋 포함, 예: `2026-10-19T12:33:00.123+09:00` |
| `hostname` | 수집 호스트 |
| `status` | `ok`, `warning`, `unknown` |
| `collection_status` | `ok`, `partial`, `error` |
| `config_status` | 일반 설정이 유효하게 적용되면 `ok`, 문서·저장 경로 적용 실패로 기존 유효 설정을 쓰면 `using_last_valid`; DB 상태와 구분 |
| `missed_intervals` | 이번 결과 전에 건너뛴 실행 주기 수 |
| `cpu` | 관측 구간 초, 사용률, iowait 비율, steal 비율, 상태 |
| `memory` | 전체·사용·가용 bytes, 사용률, 상태 |
| `filesystems` | 장치, 장치 식별자, 마운트 목록, 유형, 전체·사용·미사용·가용 bytes, 사용률, 상태 |
| `process_profile` | 현재 적용된 `is_analyzer`에 따른 `analyzer` 또는 `collector` |
| `process_status` | 대상 모두 실행 중이면 `ok`, 미실행 대상이 있으면 `warning`, 관측 불가로 존재 여부를 확정하지 못하면 `unknown` |
| `processes` | 선택한 모든 DAPP 이름을 키로 갖는 객체. 실행 중은 `true`, 미실행은 `false`, 확인 불가는 `null` |
| `collect_count` | 조회 대상, `disabled`·`pending`·`ok`·`warning`·`no_data`·`error` 상태, 비교 날짜, 조회 완료 시각, 장비 수, 경고 목록과 오류 |
| `log_silence` | DB에서 발견한 장비별 마지막 수신 시각·무수신 시간·상태와 조회 정보 |
| `log_volume` | `disabled`·`pending`·`ok`·`no_data`·`error` 상태, 집계 방식·원본 테이블·시작 포함/끝 제외 기간·실제 조회 시각·장비별 수집 건수 |
| `processing_files` | 수집 장비에서 파일 개수가 임계치 이상인 경우에만 존재. 검사 시각, 고정 검사 주기, 해당 경고 목록 |
| `errors` | component, target, code, message로 구성된 오류 목록 |

1.10.0부터 결과 JSON에서 `applied_thresholds`, `collect_count.threshold_percent`, `log_silence.default_timeout_seconds`, `log_silence.equipment[].timeout_seconds`, `processing_files.alerts[].threshold`를 제거했습니다. 임계값은 `settings.json`에서 변경하고 결과 파일에는 실제 측정값과 판정 결과를 기록합니다. `thresholds_applied_at`은 임계값 자체가 아닌 적용 시각 메타데이터이므로 유지합니다. 이전 JSON을 소급 수정하지 않으므로 결과를 읽는 프로그램은 버전에 따라 해당 필드가 없을 수 있음을 처리해야 합니다.

전체 `status`는 리소스, Java 프로세스, 일별 수집 건수 분석, 로그 무수신, 처리 파일 개수 중 하나라도 경고면 `warning`, 경고 없이 측정 불가가 있으면 `unknown`, 모두 정상 측정되고 임계치 미만이면 `ok`입니다. 분석과 `log_volume`의 `pending`, `error`, `no_data` 및 프로세스 관측의 `unknown`은 다른 항목이 모두 정상이더라도 전체 `status=unknown`으로 반영하며 `disabled`는 전체 상태에 영향을 주지 않습니다. `log_volume`에는 건수 경고 임계값이 없으며 기간 합계를 그대로 기록합니다. 처리 파일 검사의 `pending`, `partial`, `error`도 다른 경고가 없으면 전체 `unknown`에 반영하며 분석 서버에서 비활성화된 검사는 전체 상태에 영향을 주지 않습니다. `ok`, `warning`, `unknown` 모두 매 수집 주기에 JSON으로 기록합니다. `collection_status`는 CPU·메모리·스토리지 수집 상태만 나타내며 프로세스·DB 분석·파일 개수·설정 오류로 변경하지 않습니다. 예를 들어 디스크 하나가 경고이고 다른 디스크가 수집 실패이면 `status=warning`, `collection_status=partial`인 전체 결과를 저장합니다. 실행 중 발생한 수집·프로세스 관측·설정·분석·처리 파일 검사 오류는 해당 주기의 JSON `errors`와 journal에 기록하며 DB 분석 오류는 `collect_count.error`, `log_silence.error`, `log_volume.error`에도 표시합니다.

측정할 수 없는 리소스 값은 0 대신 JSON null입니다. 리소스의 이전 결과를 현재 측정값처럼 재사용하지 않습니다. DB 분석은 위에서 설명한 캐시와 `queried_at`으로 조회 시점을 구분하며, 날짜별 행이 없는 경우에만 건수 0과 존재 여부를 함께 사용합니다. 빈 파일시스템 목록은 모든 디스크 정상으로 취급하지 않습니다.

파일명은 `YYYYMMDDTHHMMSS+HHMM.json` 또는 음수 오프셋의 같은 형식을 사용합니다. UTC로 설정된 OS에서도 JSON에는 `+00:00`, 파일명에는 `+0000`을 명시하며 `Z`를 붙이지 않습니다. 시간대가 일광 절약 시간을 사용하면 해당 시각의 오프셋을 기록하므로 시계가 되돌아가 같은 지역 시각이 반복되어도 파일명을 구분할 수 있습니다. 이전 버전의 `Z` 형식도 읽으며, 모니터링 JSON 형식이 확인되면 현재 `retention` 보존 설정을 적용합니다.

## 측정 기준

CPU는 `/proc/stat`의 집계 행에서 연속한 두 샘플의 차분으로 계산합니다. 분모는 user, nice, system, idle, iowait, irq, softirq, steal의 합입니다. 분자는 user, nice, system, irq, softirq의 합입니다. guest와 guest_nice는 중복 합산하지 않습니다. 모든 CPU를 합친 0~100% 기준이며 iowait와 steal은 별도 비율입니다. 카운터가 역행하거나 관측 구간이 유효하지 않으면 그 구간은 unknown으로 처리하고 기준값을 새로 확보합니다.

메모리는 `MemTotal - MemAvailable`을 사용량으로 계산합니다. `/proc/meminfo`의 kB는 1024를 곱해 bytes로 변환합니다. CPU는 직전 관측 구간의 평균이며, 메모리와 디스크는 수집 시점의 값입니다. 수집 항목은 순차 조회되므로 완전히 동일한 순간의 원자적 OS 스냅샷은 아닙니다.

파일시스템은 mountinfo와 sysfs의 블록 장치 정보로 발견하고 열린 마운트 FD의 장치 식별을 확인한 뒤 fstatvfs로 수집합니다. LVM, NVMe, mdraid 및 일반 로컬 파일시스템을 포함합니다. Btrfs의 anonymous 장치 식별자는 source 블록 장치로 확인합니다. 같은 파일시스템 장치 식별자와 유형을 가진 bind mount는 한 항목의 mount_points로 묶습니다. 물리 디스크와 LVM을 함께 합산하지 않으며 전체 디스크 합계도 만들지 않습니다. 서브볼륨이 다른 장치 식별자를 제공하면 별도 항목으로 유지합니다.

용량 공식은 다음과 같습니다.

```text
total_bytes     = f_blocks * f_frsize
used_bytes      = (f_blocks - f_bfree) * f_frsize
free_bytes      = f_bfree * f_frsize
available_bytes = f_bavail * f_frsize
usage_percent   = used_bytes / (used_bytes + available_bytes) * 100
```

예약 공간 때문에 used + available은 total과 다를 수 있습니다. 분모가 0이면 용량은 보존하되 비율은 null, 상태는 unknown입니다.

proc, sysfs, tmpfs, overlay, 네트워크 파일시스템과 FUSE는 수집 대상에서 제외합니다. 미마운트 장치의 파일 사용량, SMART 상태, LVM thin-pool 할당량은 이 버전의 지표가 아닙니다. NFS/CIFS 수집은 장시간 블로킹을 격리할 별도 구현이 필요합니다. 프로세스가 속한 마운트 네임스페이스를 관측하므로 일반 컨테이너에서 실행한 결과를 호스트 전체 결과로 해석하면 안 됩니다.

## 실행과 저장 동작

시작 시 CPU 기준값을 확보하고 기본 60초 후 정기 수집과 임계치 판정을 수행하여 전체 결과를 저장합니다. 이후 매 수집 주기에 리소스·Java 프로세스 수집, CPU 기준값 갱신, 임계치 판정과 JSON 저장을 수행합니다. 1.11.0의 구간 수집량 조회가 완료되거나 새 오류가 발생하면 이 정기 일정 외에도 전체 결과를 추가 저장하므로 첫 파일이 60초보다 일찍 생성될 수 있습니다. 경고에서 정상으로 복구된 주기와 측정할 수 없는 항목이 있는 주기도 같은 JSON 형식으로 기록합니다. 리소스 정기 수집은 Linux CLOCK_MONOTONIC 타이머를 사용하므로 OS 시간대나 UTC/NTP 시간 변경이 주기를 바꾸지 않습니다. 구간 수집량은 달력 시각을 다시 계산하므로 시각 변경을 반영합니다. 작업 중 지나간 주기는 건너뛰고 다음 주기를 기다립니다. 실제 CPU 관측 구간은 cpu.window_seconds에 기록됩니다.

저장할 때마다 `tzset()`으로 시스템 시간대 정보를 갱신하고 JSON 시각과 파일명에 같은 시간대를 적용합니다. 기본 systemd 서비스처럼 `TZ` 환경 변수를 별도로 지정하지 않은 경우 `/etc/localtime`을 따르며, OS 시간대 변경은 데몬 재시작 없이 다음 저장부터 반영됩니다. 별도로 `TZ`를 지정해 실행하면 해당 프로세스의 시간대 설정이 우선합니다. 기존 수집 시각과 임계치 적용 시각의 실제 시점은 유지하지만, 일별 비교 날짜와 `log_volume`의 예약·DB 조회 구간은 변경된 현지 시간대를 사용합니다.

SIGTERM/SIGINT는 signalfd로 처리하며 대기 중 즉시 종료할 수 있습니다. 진행 중인 동기 파일시스템 작업은 끝난 뒤 종료 신호를 처리합니다. 저장 장치 자체가 커널에서 장시간 멈춘 경우에는 즉시 종료를 보장하지 않습니다. systemd의 종료 제한 시간은 15초입니다.

매 수집 주기에 임시 파일을 출력 디렉터리에 O_EXCL로 생성합니다. 모든 내용 기록과 파일 fsync 뒤 덮어쓰기 없는 원자적 이름 확정을 수행하고 디렉터리를 fsync합니다. 파일명이 이미 있으면 `_1`, `_2` 순번을 붙여 기존 파일을 보존합니다. 저장 실패는 journal에 기록하며, 다음 주기에 새로 수집한 결과의 저장을 다시 시도합니다. 실패한 과거 결과는 재전송하지 않습니다. 실패한 주기를 메모리에 무한 적재하지 않습니다. 완료된 과거 JSON은 아래 보존 규칙에 따라 정리합니다. crash로 남은 `.tmp`는 완료된 결과가 아니므로 자동 삭제 대상에서 제외합니다.

기본 결과 파일 권한은 0640이며 systemd의 umask 0027이 적용됩니다. `.resource-monitor.lock`은 동일 출력 디렉터리에 대한 중복 실행을 막는 잠금 파일입니다. 저장기는 잠금을 확보한 디렉터리 FD에 결과를 기록하며, 경로가 외부에서 교체되어도 다른 디렉터리를 임의로 따라가지 않습니다. 실행 중 잠금 파일을 삭제하거나 교체하지 마십시오. 이전·준비 실패 경로의 잠금 파일은 남을 수 있으며 파일 존재 자체가 잠금 보유를 뜻하지는 않습니다.

### 모니터링 JSON 보존 설정

`settings.json`의 `retention`에서 자동 삭제 여부와 보존 시간을 지정합니다. `enabled`는 JSON 불리언이고 `max_age_seconds`는 60~31536000 범위의 정수 초입니다. 기본값은 `{"enabled": true, "max_age_seconds": 3600}`이며, 항목을 생략하면 이전 1.9.0과 같은 1시간 보존을 사용합니다. 자동 삭제를 끄려면 `enabled`를 `false`로 지정합니다. 비활성 상태에서는 정리용 파일 탐색과 삭제를 수행하지 않습니다.

다음은 하루 동안 보관하는 설정 예시입니다.

```json
"retention": {
  "enabled": true,
  "max_age_seconds": 86400
}
```

활성 상태에서는 데몬 시작 시 한 번, 이후 수집 주기와 독립된 60초 타이머로 오래된 모니터링 JSON을 정리합니다. 파일명에 기록된 **수집 시각부터 `max_age_seconds` 이상** 지난 파일이 대상이며, 정확히 경계인 파일도 삭제합니다. 파일시스템의 수정 시각이나 ctime으로 생성 시각을 추정하지 않습니다. JSON 본문의 `timestamp`와 파일명의 시각이 일치하는지도 확인합니다. 시간대 오프셋을 UTC 시각으로 환산하므로 OS 시간대를 변경해도 같은 기준으로 판단합니다.

보존 설정은 다른 설정처럼 1초마다 다시 읽으며 데몬 재시작 없이 다음 정리 시점부터 반영합니다. 자동 삭제를 끄거나 보존 시간을 늘리면 이후 검사에 새 값을 사용합니다. 다시 켜거나 보존 시간을 줄이면 다음 60초 정리에서 이미 존재하는 오래된 파일도 새 기준으로 삭제합니다. 잘못된 값은 마지막 유효 설정을 유지하고 `config_status=using_last_valid`를 기록합니다. DB가 재연결 중이거나 조회에 실패해도 유효한 보존 설정 변경은 반영됩니다.

삭제 범위는 현재 적용된 `collection.output_directory`(기본 `/Monitor`)의 직계 파일입니다. 역할이 분석 또는 수집인지와 관계없이 같은 규칙을 적용합니다. 출력 경로를 변경하면 다음 정리부터 새 경로만 검사하며 이전 경로의 남은 파일을 계속 삭제하지 않습니다. 정리 간격은 60초로 고정되어 있으며 수집 주기를 더 길게 바꾸어도 유지됩니다. 실행 중 작업이나 대량 과거 파일 검사에 시간이 걸리면 실제 정리 완료가 늦어질 수 있습니다.

`YYYYMMDDTHHMMSS+HHMM.json`, 음수 오프셋, 기존 `Z` 형식과 `_1`~`_9999` 충돌 접미사를 인식합니다. 유효한 날짜와 모니터링 JSON 필수 구조를 확인한 16MiB 이하의 일반 파일만 삭제합니다. 다른 JSON, 내용이 손상되거나 시각이 일치하지 않는 파일, 미래 시각 파일, 하위 폴더, 심볼릭 링크, 하드링크, 잠금 파일과 `.tmp`는 보존합니다. `/CloudESM/data/processing/collector`와 `normalize`의 `.tmp`·`.done`은 삭제하지 않습니다.

삭제 성공 시 journal에 `snapshots_pruned count=N`, 실패 시 `component=snapshot_retention` 오류를 남깁니다. 삭제 실패는 리소스 수집과 새 JSON 저장을 중단시키지 않으며 다음 정리 시 다시 시도합니다. 이 유지관리 결과는 현재 JSON 스키마에 새 필드를 추가하지 않고 journal에 기록합니다.

```bash
journalctl -u resource-monitor --since '-10 minutes' --no-pager
```

기본 60초 정기 수집에서는 하루 약 1,440개 파일을 생성하고, 분석 서버는 구간 수집량 결과 갱신에 따른 파일이 추가될 수 있습니다. 기본 보존 설정에서는 통상 최근 1시간 분량을 유지합니다. 보존 시간을 늘리거나 자동 삭제를 끄면 그만큼 파일이 누적됩니다. 업그레이드 후 첫 실행부터 이미 존재하는 정상 모니터링 JSON에도 현재 보존 설정이 적용되므로, 보관 요구에 맞는 값을 시작 전에 지정하십시오.

## 수동 진단

```bash
mkdir -p /tmp/resource-monitor-output
./build/release/resource-monitor --config ./config/settings.json --output /tmp/resource-monitor-output --interval-seconds 1 --once
```

`--once`도 CPU 차분을 위해 지정한 관측 구간을 기다린 후 한 번 수집·판정하고 전체 결과를 하나의 JSON 파일로 저장합니다. `--interval-seconds`는 1~86400 범위이며 서비스 기본값은 60입니다. `--help`와 `--version`도 제공합니다. 수동 진단은 service 계정과 동일한 권한 조건에서도 확인하십시오.

## 테스트와 RPM

```bash
sudo dnf install -y gcc-toolset-15-libasan-devel gcc-toolset-15-libubsan-devel
cmake --preset sanitizers
cmake --build --preset sanitizers --parallel 2
ctest --preset sanitizers
python3 scripts/package_source.py
```

RPM은 빌드할 OL8 또는 OL9 시스템에서 다음처럼 생성합니다. Source1은 공식 JSON 배포본이며 spec에서도 해시를 다시 검증합니다.

```bash
sudo dnf install -y rpm-build gcc-toolset-15-gcc-c++ cmake make python3 pkgconf-pkg-config mariadb-connector-c-devel
mkdir -p "$HOME/rpmbuild/SOURCES" "$HOME/rpmbuild/SPECS"
cp artifacts/resource-monitor-1.11.0.tar.gz "$HOME/rpmbuild/SOURCES/"
curl --fail --location https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz --output "$HOME/rpmbuild/SOURCES/json.tar.xz"
cp packaging/rpm/resource-monitor.spec "$HOME/rpmbuild/SPECS/"
rpmbuild -ba "$HOME/rpmbuild/SPECS/resource-monitor.spec"
```

RPM 설치는 전용 계정, `/Monitor` 디렉터리와 `root:resource-monitor` 소유의 권한 0750인 `/etc/resource-monitor/secrets` 디렉터리를 생성합니다. 실제 DB 계정 파일은 설치하거나 덮어쓰지 않습니다. MariaDB Connector/C 런타임 의존성은 RPM 공유 라이브러리 의존성으로 해결합니다. 서비스 자동 시작은 설치 후 관리자가 `systemctl enable --now resource-monitor`로 선택합니다. 사용자 설정은 `%config(noreplace)`로 보존하며 `/Monitor`의 결과 파일은 패키지 소유 파일로 등록하지 않습니다. 이전 파일을 이전할 때 `thresholds.json.legacy`에 원본도 보관하며 기존 백업은 덮어쓰지 않습니다. 이후 편집 대상은 `settings.json`입니다. 패키지 기본 예제가 `.rpmnew`로 남을 수 있으므로 필요한 새 항목만 참고해 기존 설정에 추가하십시오.

Docker 개발 환경은 다음처럼 만들 수 있습니다. 컨테이너 테스트는 Oracle Linux 커널 및 실제 systemd 부팅 검증과 구분해야 합니다.

```bash
docker build -f packaging/Dockerfile.build --build-arg ORACLE_VERSION=8 -t resource-monitor-build:ol8 .
docker build -f packaging/Dockerfile.build --build-arg ORACLE_VERSION=9 -t resource-monitor-build:ol9 .
```

실제 운영 승인에는 대상 서버의 OL 마이너 버전, RHCK/UEK, 마운트 구성, SELinux Enforcing 환경에서 설치·기동·60초 수집·상태별 JSON 저장·재시작을 확인하는 절차가 필요합니다. 테스트는 CPU 경계, 임계치 재로드, 매 주기 저장, 예약 공간, 부분 실패, 원자 저장 실패, 마운트 파싱, FD 정리, 종료 신호 외에 일별 건수 증감 경계, 기준값 0, 날짜별 누락, 잘못된 DB 값, 결과 개수 제한, OS 날짜 계산, 분석 설정 재로드, 비동기 조회와 DB 비활성화를 검증합니다. 구간 수집량은 기간 경계·분류별 합산·윤년·일광 절약 시간·설정 변경·동일 구간의 쿼리 재사용·연결 실패·결과 JSON도 검사합니다.

`tests/log_volume_database_integration_test.py`는 별도 MariaDB에 시험 데이터를 준비하는 선택적 통합 테스트이며 일반 CTest에는 포함하지 않습니다. 실제 업무 DB에서 실행하면 안 됩니다. 명시적으로 허용한 `window-db` 호스트, 전용 사설망, 서버 이름, 검증 토큰, 관리자 설정과 조회 전용 계정이 모두 일치해야만 시험 테이블을 재생성합니다. 배포 데몬의 DB 동작은 이 시험 데이터 준비 코드와 별개로 조회 전용입니다.

<a id="upgrade-from-1-8"></a>

## 1.8.0 제거 후 1.10.0 재설치 상세 가이드

이 절차는 `resource-monitor-1.8.0-install-kit-20261023.zip`으로 RPM 설치한 Oracle Linux 8/9 x86_64 장비를 **2026-10-27 당시 버전 1.10.0**으로 이전하는 보관용 가이드입니다. 아래 명령과 배포 파일명은 당시 버전을 유지하며, 현재 개발 버전 1.11.0의 기능 설명과 구분합니다. 기존 패키지를 제거한 다음 새 RPM을 설치하고, 장비의 기존 설정과 DB 계정을 복원합니다. 작업 중에는 모니터링이 중단됩니다. CloudESM 서비스나 MariaDB 서버를 중지할 필요는 없습니다.

제거·설치 대상은 `resource-monitor` 패키지뿐입니다. DB 사용자·비밀번호, DB 테이블, `/CloudESM` 처리 파일을 삭제하지 않습니다. 기존 ZIP의 역할별 예제를 다시 복사하지 않고 **해당 장비에서 사용하던 설정**을 기준으로 이전합니다. 이 절차는 운영자가 수행하는 안내이며, 문서 추가 작업에서 운영 장비를 다시 설치하거나 재부팅한 것은 아닙니다.

### 1. 준비 파일과 실행 환경

서버의 `/root`에 아래 두 ZIP을 준비합니다. 원래 1.8.0 ZIP은 문제 발생 시 복구할 RPM을 확보하기 위한 것입니다. 가이드가 추가된 ZIP을 사용하거나 다른 경로에 업로드했다면 아래 변수의 파일명을 실제 이름으로 바꿉니다.

- 기존: `resource-monitor-1.8.0-install-kit-20261023.zip`
- 최신: `resource-monitor-1.10.0-update-20261027.zip`

이하 `bash` 코드는 **DB의 `MariaDB>` 프롬프트가 아닌 서버의 root Bash 셸**에서 실행합니다. 같은 셸에서 순서대로 진행합니다. 중요 명령의 `|| exit 1`은 실패 시 현재 셸을 종료해 후속 제거·시작을 막습니다. 종료됐다면 오류 원인을 해결한 뒤 재접속하고 기록한 작업·백업 경로로 변수를 복구하십시오. 단계별 확인이 끝나기 전에는 다음 단계로 넘어가지 않습니다. 절차의 JSON 편집·검사에만 Python 3가 필요하며 데몬 실행 자체에는 필요하지 않습니다. 폐쇄망은 `unzip`, Python 3와 해당 OS용 RPM 의존성을 사내 저장소 또는 별도 RPM으로 먼저 준비합니다.

```bash
test "$(id -u)" -eq 0 || exit 1
id -u
cat /etc/oracle-release
uname -m
rpm -q resource-monitor
/usr/sbin/resource-monitor --version
systemctl cat resource-monitor
if [ -e /etc/resource-monitor/.settings.json.migrate ] || [ -L /etc/resource-monitor/.settings.json.migrate ]; then
    printf "이전 설정 이전 파일을 보존하고 확인한 후 진행하십시오: /etc/resource-monitor/.settings.json.migrate\n" >&2
    exit 1
fi
runuser -u resource-monitor -- /usr/sbin/resource-monitor --check-config || exit 1
dnf install -y unzip python3 || exit 1
```

UID는 `0`, 아키텍처는 `x86_64`, 기존 버전은 `1.8.0`인지 확인합니다. `rpm -q`가 실패하거나 실행 경로가 `/usr/sbin/resource-monitor`가 아닌 소스 설치 장비에는 이 제거 절차를 그대로 사용하지 않습니다. `systemctl cat`에서 별도 `ExecStart`, `--config`, `--output`, `--interval-seconds` 또는 drop-in이 있는지도 확인합니다. 아래는 기본 설정 경로 `/etc/resource-monitor/settings.json` 기준이며, 사용자 정의 실행 옵션이 있으면 실제 경로와 우선 적용값을 확인해 맞춰야 합니다.

```bash
rm_old_zip=/root/resource-monitor-1.8.0-install-kit-20261023.zip
rm_new_zip=/root/resource-monitor-1.10.0-update-20261027.zip
test -f "$rm_old_zip" && test -f "$rm_new_zip" || exit 1
rm_work=$(mktemp -d /root/resource-monitor-upgrade-20261027.XXXXXX) || exit 1
unzip -q "$rm_old_zip" -d "$rm_work" || exit 1
unzip -q "$rm_new_zip" -d "$rm_work" || exit 1
rm_old_dir="$rm_work/resource-monitor-1.8.0"
rm_new_dir="$rm_work/resource-monitor-1.10.0-update-20261027"
(cd "$rm_old_dir" && sha256sum -c SHA256SUMS.txt) || exit 1
(cd "$rm_new_dir" && sha256sum -c SHA256SUMS.txt) || exit 1
. /etc/os-release
case "$ID:${VERSION_ID%%.*}:$(uname -m)" in
    ol:8:x86_64) rm_el=8 ;;
    ol:9:x86_64) rm_el=9 ;;
    *) printf '지원 대상 Oracle Linux 8/9 x86_64가 아닙니다.\n' >&2; exit 1 ;;
esac
rm_old_rpm="$rm_old_dir/packages/resource-monitor-1.8.0-1.el${rm_el}.x86_64.rpm"
rm_new_rpm="$rm_new_dir/packages/resource-monitor-1.10.0-1.el${rm_el}.x86_64.rpm"
rpm -qp "$rm_old_rpm" "$rm_new_rpm" || exit 1
rpm -K "$rm_old_rpm" "$rm_new_rpm" || exit 1
rpm -Uvh --test "$rm_new_rpm" || exit 1
```

모든 파일의 SHA-256 및 RPM digest 검사와 설치 사전검사가 성공해야 합니다. `rpm -K`의 digest 검증은 배포자 서명 검증과 구분됩니다. 의존성 부족이 나오면 제거 전에 해결합니다. EL8 장비에는 EL8 RPM, EL9 장비에는 EL9 RPM을 사용하고 `--nodeps`, `--force`는 사용하지 않습니다. 셸을 다시 열면 `rm_work`, `rm_old_rpm`, `rm_new_rpm` 등의 변수를 같은 경로로 다시 지정해야 합니다.

### 2. 중지 전 상태와 실제 경로 기록

백업은 결과 저장 경로 밖의 `/var/backups/resource-monitor`에 root 전용으로 생성합니다. 다음 코드는 비밀번호를 출력하지 않고 설정의 파일 경로만 읽습니다.

```bash
umask 077
rm_backup="/var/backups/resource-monitor/$(date +%Y%m%dT%H%M%S)-1.8.0-to-1.10.0"
mkdir -p -- "$rm_backup" || exit 1
chmod 0700 "$rm_backup" || exit 1
rm_backup=$(readlink -f -- "$rm_backup") || exit 1
rpm -q resource-monitor > "$rm_backup/package-before.txt"
systemctl is-enabled resource-monitor > "$rm_backup/enabled-before.txt"
systemctl is-active resource-monitor > "$rm_backup/active-before.txt"
systemctl cat resource-monitor > "$rm_backup/unit-before.txt"
rm_output=$(python3 -c 'import json; s=json.load(open("/etc/resource-monitor/settings.json")); print(s.get("collection",{}).get("output_directory","/Monitor"))') || exit 1
rm_credentials=$(python3 -c 'import json; s=json.load(open("/etc/resource-monitor/settings.json")); print(s.get("mysql",{}).get("credentials_file","/etc/resource-monitor/secrets/mysql.json"))') || exit 1
rm_ca=$(python3 -c 'import json; s=json.load(open("/etc/resource-monitor/settings.json")); print(s.get("mysql",{}).get("tls_ca_file") or "")') || exit 1
rm_analyzer=$(python3 -c 'import json; s=json.load(open("/etc/resource-monitor/settings.json")); print("true" if s.get("is_analyzer",False) else "false")') || exit 1
rm_tls_mode=$(python3 -c 'import json; s=json.load(open("/etc/resource-monitor/settings.json")); print(s.get("mysql",{}).get("tls_mode","verify_identity"))') || exit 1
printf 'output=%s\ncredentials_file=%s\ntls_ca_file=%s\n' "$rm_output" "$rm_credentials" "$rm_ca" > "$rm_backup/paths-before.txt"
printf '백업 위치: %s\n출력 위치: %s\n' "$rm_backup" "$rm_output"
du -sh -- "$rm_output" /etc/resource-monitor
df -h -- "$rm_output" "$rm_backup"
```

원래 비활성 서비스라면 상태 조회가 0이 아닌 종료 코드를 반환할 수 있습니다. 기록된 상태를 확인하십시오. `--output`을 별도 지정한 서비스는 실제 출력 경로로 `rm_output`을 고쳐서 다음 단계에 사용합니다. 출력 파일 전체를 백업할 공간이 없으면 별도 백업 저장소를 확보한 후 진행합니다. 다른 위치의 계정·CA 파일도 아래 백업에 포함됩니다. 환경 파일이나 별도 systemd 단위가 참조하는 추가 파일은 그 파일까지 별도로 보존합니다. 아래 백업 코드는 계정·CA 경로가 심볼릭 링크라면 링크와 실제 대상 파일을 함께 보존합니다. 비활성 역할이나 TLS 비활성화로 사용하지 않는 파일이 없으면 생략하며, 분석 장비가 실제 사용하는 파일이 없으면 중단합니다.

### 3. 서비스 중지와 설정·기존 JSON·구버전 RPM 백업

```bash
systemctl stop resource-monitor || exit 1
systemctl is-active resource-monitor
systemctl show resource-monitor -p MainPID
test "$(systemctl show resource-monitor -p MainPID --value)" = 0 || exit 1
```

`inactive`와 `MainPID=0`을 확인한 뒤 계속합니다. 중지가 완료되지 않았으면 패키지 제거와 출력 폴더 백업을 진행하지 않습니다.

```bash
rm_config_paths=(etc/resource-monitor)
for rm_path in "$rm_credentials" "$rm_ca"; do
    case "$rm_path" in
        "") continue ;;
        /*) ;;
        *) printf '설정 파일 경로가 절대 경로가 아닙니다.\n' >&2; exit 1 ;;
    esac
    if [ ! -e "$rm_path" ]; then
        if [ "$rm_analyzer" = true ] && { [ "$rm_path" = "$rm_credentials" ] || [ "$rm_tls_mode" != disabled ]; }; then
            printf '사용 중인 설정 파일이 없습니다: %s\n' "$rm_path" >&2
            exit 1
        fi
        printf '존재하지 않는 미사용 파일은 생략합니다: %s\n' "$rm_path"
        continue
    fi
    case "$rm_path" in
        /etc/resource-monitor/*) ;;
        *) rm_config_paths+=("${rm_path#/}") ;;
    esac
    rm_real_path=$(readlink -f -- "$rm_path") || exit 1
    if [ "$rm_real_path" != "$rm_path" ]; then
        case "$rm_real_path" in
            /etc/resource-monitor/*) ;;
            *) rm_config_paths+=("${rm_real_path#/}") ;;
        esac
    fi
done
for rm_path in etc/systemd/system/resource-monitor.service etc/systemd/system/resource-monitor.service.d; do
    if [ -e "/$rm_path" ]; then
        rm_config_paths+=("$rm_path")
    fi
done
tar --acls --xattrs --selinux -czpf "$rm_backup/configuration.tar.gz" -C / "${rm_config_paths[@]}" || exit 1
test -d "$rm_output" || exit 1
rm_output=$(readlink -f -- "$rm_output") || exit 1
case "$rm_output" in
    ""|/) printf '출력 경로를 다시 확인하십시오.\n' >&2; exit 1 ;;
esac
case "$rm_backup/" in
    "$rm_output/"*) printf '백업 폴더는 출력 경로 밖에 있어야 합니다.\n' >&2; exit 1 ;;
esac
printf '%s\n' "$rm_output" > "$rm_backup/output-directory.txt"
tar --acls --xattrs --selinux -czpf "$rm_backup/monitor-output.tar.gz" -C "$rm_output" . || exit 1
cp -a -- /etc/resource-monitor/settings.json "$rm_backup/settings.before.json" || exit 1
cp -a -- "$rm_old_rpm" "$rm_backup/" || exit 1
(cd "$rm_backup" && sha256sum configuration.tar.gz monitor-output.tar.gz settings.before.json "$(basename "$rm_old_rpm")" > SHA256SUMS.txt) || exit 1
(cd "$rm_backup" && sha256sum -c SHA256SUMS.txt) || exit 1
tar -tzf "$rm_backup/configuration.tar.gz" || exit 1
tar -tzf "$rm_backup/monitor-output.tar.gz" > "$rm_backup/monitor-output.list.txt" || exit 1
printf '복구에 사용할 백업 경로: %s\n' "$rm_backup"
```

모든 명령과 백업 검증이 성공해야 합니다. 백업 경로를 별도 작업 기록에 남깁니다. `configuration.tar.gz`에는 실제 DB 비밀번호가 포함될 수 있으므로 root 전용으로 유지하고 메일·일반 배포 ZIP에 포함하지 않습니다. 백업 과정은 DB 내용을 덤프하거나 변경하지 않습니다.

### 4. 기존 1.8.0 패키지 제거

제거 대상과 의존 패키지를 먼저 확인합니다. `--assumeno` 명령은 확인 후 취소되므로 0이 아닌 종료 코드는 예상된 결과입니다.

```bash
dnf remove --assumeno --setopt=clean_requirements_on_remove=False resource-monitor
```

예상 밖의 패키지가 함께 제거되는 거래라면 중단하고 의존 관계를 확인합니다. 제거 대상이 확인되면 실행합니다. 자동 의존성 정리를 끄므로 MariaDB Connector/C 등 기존 런타임 의존성을 불필요하게 제거하지 않습니다.

```bash
dnf remove --setopt=clean_requirements_on_remove=False resource-monitor || exit 1
rpm -q resource-monitor
```

마지막 명령의 `package resource-monitor is not installed`는 이 단계의 정상 결과입니다. `%config(noreplace)`라도 **제거 후 재설치가 기존 설정의 자동 복원을 보장하지는 않습니다.** 수정된 파일은 `.rpmsave`로 남을 수 있고, 수정하지 않은 기본 설정은 제거될 수 있습니다. 다음 단계에서 백업으로 직접 복원합니다. 서비스 계정, 남아 있는 secrets와 결과 파일을 따로 삭제하거나 `/etc/resource-monitor`, `/Monitor`에 `rm -rf`를 실행하지 않습니다.

### 5. 최신 RPM 설치와 기존 설정 복원

```bash
dnf install "$rm_new_rpm" || exit 1
rpm -q resource-monitor
/usr/sbin/resource-monitor --version
getent passwd resource-monitor
tar --acls --xattrs --selinux -xzpf "$rm_backup/configuration.tar.gz" -C / || exit 1
cmp -- "$rm_backup/settings.before.json" /etc/resource-monitor/settings.json || exit 1
systemctl daemon-reload || exit 1
```

`resource-monitor 1.10.0`과 설정 원본 일치를 확인합니다. 이 시점에는 **아직 서비스를 시작하지 않습니다.** 복원 대상은 설정·계정·CA·기존 사용자 정의 단위 파일이며 새 RPM의 `/usr/sbin/resource-monitor`와 `/usr/lib/systemd/system/resource-monitor.service`를 구버전으로 덮어쓰지 않습니다. 사용자 정의 단위가 있다면 `systemctl cat resource-monitor`로 새 바이너리를 실행하는지 다시 확인합니다. 기존 결과 폴더가 보존되어 있다면 JSON 백업을 그 위에 풀 필요가 없습니다.

### 6. 1.10.0 설정 이전과 최초 자동 삭제 방지

**1.8.0은 자동 삭제가 없었지만 1.10.0은 `retention` 생략 시 기본 1시간 보존입니다.** 기존 파일을 유지하며 전환할 수 있도록 아래에서는 첫 실행의 자동 삭제를 명시적으로 끕니다. 설치 확인 후 운영 보존 정책을 선택합니다. 기존 역할, 수집 주기, 저장 경로, DB 연결·계정 경로와 모든 임계값은 유지합니다.

```bash
install -m 0640 -o root -g resource-monitor /etc/resource-monitor/settings.json /etc/resource-monitor/settings.json.new || exit 1
python3 - /etc/resource-monitor/settings.json.new <<'PY' || exit 1
import json
import sys
path = sys.argv[1]
with open(path, encoding="utf-8") as stream:
    settings = json.load(stream)
settings["retention"] = {"enabled": False, "max_age_seconds": 3600}
profiles = settings.get("process_monitor", {})
if "analyzer" in profiles:
    renamed = ["AnalyzeEngine" if name == "SoarAnalyzerEngine" else name for name in profiles["analyzer"]]
    profiles["analyzer"] = list(dict.fromkeys(renamed))
with open(path, "w", encoding="utf-8") as stream:
    json.dump(settings, stream, ensure_ascii=False, indent=2)
    stream.write("\n")
PY
diff -u "$rm_backup/settings.before.json" /etc/resource-monitor/settings.json.new
runuser -u resource-monitor -- /usr/sbin/resource-monitor --config /etc/resource-monitor/settings.json.new --check-config && mv -f -- /etc/resource-monitor/settings.json.new /etc/resource-monitor/settings.json || exit 1
```

`diff`는 변경이 있으면 종료 코드 1을 반환합니다. 자동 삭제 비활성화와 `SoarAnalyzerEngine` → `AnalyzeEngine` 이름 변경·중복 제거, JSON 정렬 형태 외의 운영값이 유지되었는지 확인합니다. 기존 `Collector`/`CloudESMCollector` 등 다른 DAPP 이름은 그대로 유지합니다. 실제 장비에 맞는 목록을 확인하되 전체 샘플 프로필로 덮어쓰지 않습니다.

`Configuration is valid`가 출력되고 설정 교체까지 성공해야 합니다. 실패하면 `.new` 파일과 자격증명 접근 권한을 수정한 뒤 다시 검증합니다. `--check-config`는 설정 문법·값과 분석 장비의 계정 파일을 검사하며 실제 DB TCP 접속, TLS 연결 성공, 결과 저장 권한까지 보증하지 않습니다. `is_analyzer=false`인 수집 장비는 계정 파일 검사와 DB 연결을 수행하지 않습니다.

기본 계정 파일을 사용하는 분석 장비의 권한이 잘못된 경우 다음처럼 맞춥니다. 사용자 정의 계정 경로는 해당 경로에 같은 원칙을 적용하고 비밀번호는 변경하지 않습니다.

```bash
chown root:resource-monitor /etc/resource-monitor/secrets || exit 1
chmod 0750 /etc/resource-monitor/secrets || exit 1
chown root:resource-monitor /etc/resource-monitor/secrets/mysql.json || exit 1
chmod 0640 /etc/resource-monitor/secrets/mysql.json || exit 1
```

출력 디렉터리가 없을 때만 생성합니다. 기존 디렉터리·하위 파일의 소유권을 일괄 변경하지 않습니다. 권한 확인이 실패하면 해당 경로의 마운트·상위 디렉터리·ACL·SELinux 정책을 점검합니다.

```bash
if [ ! -d "$rm_output" ]; then
    install -d -m 0750 -o resource-monitor -g resource-monitor -- "$rm_output" || exit 1
fi
namei -l -- "$rm_output"
runuser -u resource-monitor -- test -x "$rm_output" || exit 1
runuser -u resource-monitor -- test -w "$rm_output" || exit 1
runuser -u resource-monitor -- /usr/sbin/resource-monitor --check-config || exit 1
```

### 7. 분석 장비의 무수신 조회 인덱스 확인

`is_analyzer=true`인 장비에만 해당합니다. 기존 DB 계정을 다시 만들거나 비밀번호를 바꿀 필요는 없습니다. 조회 계정의 두 모니터링 테이블 `SELECT` 권한을 유지합니다. DB 서버의 관리자 SQL 세션에서 먼저 확인합니다.

```sql
SHOW INDEX FROM CloudESM.SUM_COLLECT_INFO;
```

사용 가능한 전체 열 BTREE 인덱스의 선두 열이 `(eqp_ip, recv_time, count)`이면 추가하지 않습니다. 해당 인덱스가 없을 때만 DBA가 테이블 크기, 여유 공간, DDL 지원 여부와 작업 시간을 검토한 뒤 제공 SQL을 실행합니다. 같은 이름의 인덱스가 다른 정의로 있으면 `IF NOT EXISTS`가 이를 고치지 않으므로 먼저 확인합니다. DB 서버의 Linux 셸에서 로컬 관리자 소켓 접속을 사용하는 예시는 다음과 같습니다.

```bash
mariadb -u root -p CloudESM < /usr/share/resource-monitor/sql/log_silence_index.sql
```

원격 DB는 SQL 파일을 DBA에게 전달하고 실제 접속 주소·포트에 맞춰 실행합니다. 인덱스 생성은 별도 DB 작업이며 RPM과 데몬이 자동 수행하지 않습니다. 인덱스가 없으면 기존 서버 측 집계를 사용해 기능은 유지됩니다. 인덱스가 있으면 장비별 `count > 0`의 최신 수신 행만 반환하는 쿼리를 사용합니다. 서버에서 보관 중인 오래 무수신인 장비도 감시 대상에 남습니다. 인덱스는 연결 시 확인하므로 이 단계에서 적용하면 다음 시작에 반영됩니다.

### 8. 서비스 시작과 새 JSON 검증

```bash
systemctl daemon-reload || exit 1
systemctl reset-failed resource-monitor || exit 1
python3 -c 'import json; s=json.load(open("/etc/resource-monitor/settings.json")); assert s.get("retention",{}).get("enabled") is False, "First-start retention must be disabled"' || exit 1
touch "$rm_backup/start-marker" || exit 1
systemctl enable --now resource-monitor || exit 1
systemctl is-enabled resource-monitor
systemctl is-active resource-monitor
systemctl status resource-monitor --no-pager
/usr/sbin/resource-monitor --version
journalctl -u resource-monitor --since '-10 minutes' --no-pager
```

시작 직전 Python 검사는 실제 적용 파일의 자동 삭제가 꺼져 있는지 다시 확인합니다. 검사가 실패하면 보존 설정을 먼저 수정하고 시작 명령을 실행하지 않습니다. `enabled`, `active`, `resource-monitor 1.10.0`을 확인합니다. 첫 파일은 시작 후 설정된 수집 주기만큼 지난 뒤 생성됩니다. 기본이면 약 60초이며, 수집 작업 시간이 추가될 수 있습니다. 최소 두 수집 주기에 걸쳐 파일이 계속 생성되는지 확인합니다. 아래 명령은 **이번 시작 이후** 생성·수정된 JSON만 선택하여 이전 파일을 성공 증거로 잘못 사용하지 않도록 합니다.

```bash
find "$rm_output" -maxdepth 1 -type f -name '*.json' -newer "$rm_backup/start-marker" -printf '%TY-%Tm-%Td %TH:%TM:%TS %f\n' | sort
rm_latest=$(find "$rm_output" -maxdepth 1 -type f -name '*.json' -newer "$rm_backup/start-marker" -printf '%T@ %p\n' | sort -nr | head -n 1 | cut -d' ' -f2-)
if [ -n "$rm_latest" ]; then
    python3 -m json.tool "$rm_latest"
else
    printf '이번 시작 이후 JSON이 없습니다. 수집 주기와 journal을 확인하십시오.\n' >&2
fi
```

| 확인 항목 | 판단 기준 |
| --- | --- |
| 시각·출력 | 이번 서비스 시작 이후의 `timestamp`, OS 시간대 및 지정한 저장 경로 |
| 설정·리소스 | `config_status=ok`, CPU·메모리·스토리지 값과 `collection_status`, `errors` 확인 |
| 역할·프로세스 | 분석은 `process_profile=analyzer`, 수집은 `collector`; DAPP 이름과 true/false 확인 |
| 분석 DB | `collect_count`·`log_silence`의 상태와 `queried_at`이 갱신되는지 확인; 오류는 각 `error`와 최상위 `errors` 확인 |
| 수집 DB | `collect_count.status=disabled`, `log_silence.status=disabled` |
| JSON 계약 | 임계값 숫자 필드가 없어도 정상; 실제 측정값과 판정 결과, `thresholds_applied_at`은 유지 |
| 보존 | 최초 전환 단계에서는 `retention.enabled=false`이므로 과거 파일 자동 삭제 없음 |

장비 무수신, 임계치 초과 또는 DAPP 미실행에 따른 `warning`은 데몬 설치 실패와 구분합니다. DB 접속 실패는 `error`로 기록되지만 리소스 JSON은 계속 생성되어야 합니다. 최초 비동기 조회의 `pending`이 계속되거나 `error`가 남으면 실제 DB 연결·권한·쿼리 실행 시간을 확인합니다. 수집 서버에서 처리 파일 수가 임계치 미만이면 `processing_files` 항목이 없는 것이 정상입니다.

1.10.0 결과에서 제거된 필드는 `applied_thresholds`, `collect_count.threshold_percent`, `log_silence.default_timeout_seconds`, `log_silence.equipment[].timeout_seconds`, `processing_files.alerts[].threshold`입니다. `schema_version`은 1을 유지하므로 외부 수집기나 대시보드가 이 필드를 필수로 읽고 있다면 함께 수정해야 합니다. 기존 JSON은 다시 작성하지 않습니다.

### 9. 검증 후 보존 기간 선택

자동 삭제를 계속 끄려면 현재 `retention.enabled=false`를 유지합니다. 하루 보존을 원하면 `/etc/resource-monitor/settings.json`의 **기존 `retention` 객체를** 다음과 같이 수정합니다. 최상위 객체에 같은 키를 중복 추가하지 않습니다.

```json
"retention": {
  "enabled": true,
  "max_age_seconds": 86400
}
```

1시간은 `3600`, 하루는 `86400`, 7일은 `604800`초입니다. 허용 범위는 60~31536000의 정수입니다. 약 1초마다 설정을 다시 읽으므로 데몬 재시작은 필요하지 않습니다. 활성화하면 다음 60초 정리에서 현재 출력 경로의 **기존 파일에도** 보존 기준이 적용됩니다. 필요한 과거 파일은 먼저 출력 경로 밖에 보관합니다. 삭제 대상은 파일명·본문 수집 시각과 모니터링 구조가 검증된 직계 JSON이며 CloudESM의 `.tmp`·`.done` 파일은 대상이 아닙니다.

```bash
journalctl -u resource-monitor --since '-10 minutes' --no-pager
```

정리가 발생하면 `snapshots_pruned count=N`을 확인할 수 있습니다. 삭제할 파일이 없거나 삭제가 비활성 상태라면 해당 로그가 없는 것이 정상입니다.

### 10. 재부팅 확인과 작업 완료 기록

`enable --now`는 자동 시작 설정과 현재 실행을 적용합니다. 실제 재부팅 테스트는 장비 전체에 영향을 주므로 운영 점검 시간에 별도로 수행합니다. 재부팅 후 최소 한 수집 주기를 기다려 다음을 확인합니다.

```bash
systemctl is-enabled resource-monitor
systemctl is-active resource-monitor
journalctl -u resource-monitor -b --no-pager
find "$rm_output" -maxdepth 1 -type f -name '*.json' -newermt "$(uptime -s)" -printf '%TY-%Tm-%Td %TH:%TM:%TS %f\n' | sort
```

재접속하면 셸 변수가 사라지므로 `rm_output`을 실제 설정 경로로 다시 지정합니다. 이번 부팅 이후의 JSON 시각과 내용을 확인하고, 적용 버전·보존 정책·백업 경로·DB 및 DAPP 확인 결과를 작업 기록에 남깁니다. 백업은 운영 확인 기간 동안 보호된 위치에 보관합니다.

### 11. 실패 시 1.8.0으로 복구

새 버전이 정상 동작하지 않고 현장에서 바로 해결할 수 없다면 아래 순서로 복구합니다. 재접속했다면 2단계에서 기록한 실제 백업 경로를 먼저 지정합니다. 아래 경로의 날짜·시각은 설명용이므로 그대로 실행하지 말고 실제 값으로 바꿉니다.

```bash
rm_backup=/var/backups/resource-monitor/실제백업폴더명
umask 077
test -f "$rm_backup/configuration.tar.gz" || exit 1
(cd "$rm_backup" && sha256sum -c SHA256SUMS.txt) || exit 1
. /etc/os-release
rm_el=${VERSION_ID%%.*}
rm_rollback_rpm="$rm_backup/resource-monitor-1.8.0-1.el${rm_el}.x86_64.rpm"
test -f "$rm_rollback_rpm" || exit 1
systemctl stop resource-monitor || exit 1
systemctl show resource-monitor -p MainPID
test "$(systemctl show resource-monitor -p MainPID --value)" = 0 || exit 1
```

`MainPID=0`과 백업 검증 성공을 확인한 뒤 현재 설정·오류 로그도 보존하고 구버전으로 교체합니다. 제거 거래에서 예상 밖의 패키지가 대상이면 진행하지 않습니다.

```bash
cp -a -- /etc/resource-monitor/settings.json "$rm_backup/settings.failed-1.10.0.json" || exit 1
journalctl -u resource-monitor -n 300 --no-pager > "$rm_backup/journal.failed-1.10.0.txt"
dnf remove --setopt=clean_requirements_on_remove=False resource-monitor || exit 1
dnf install "$rm_rollback_rpm" || exit 1
tar --acls --xattrs --selinux -xzpf "$rm_backup/configuration.tar.gz" -C / || exit 1
cmp -- "$rm_backup/settings.before.json" /etc/resource-monitor/settings.json || exit 1
/usr/sbin/resource-monitor --version
runuser -u resource-monitor -- /usr/sbin/resource-monitor --check-config || exit 1
systemctl daemon-reload || exit 1
systemctl reset-failed resource-monitor || exit 1
systemctl enable --now resource-monitor || exit 1
systemctl is-active resource-monitor
journalctl -u resource-monitor --since '-10 minutes' --no-pager
```

**1.8.0은 `retention` 키를 지원하지 않으므로 1.10.0 설정 파일을 그대로 사용하지 않습니다.** 반드시 제거 전 원본 설정·secrets를 복원하고 `resource-monitor 1.8.0`, 설정 검사 성공 및 새 JSON 생성을 확인합니다. 위 명령은 원래 상시 실행하던 장비 기준으로 자동 시작을 다시 활성화합니다. 원래 비활성 장비였다면 기록한 `enabled-before.txt`, `active-before.txt`에 맞게 상태를 복원합니다.

기존 출력 폴더가 그대로 있다면 JSON 백업을 다시 풀지 않습니다. 보존 설정으로 삭제된 과거 결과가 필요하면 아래처럼 **별도 폴더에** 복구하고 확인합니다. 실행 중 출력 폴더의 잠금 파일을 덮어쓰지 않습니다.

```bash
rm_restore_dir=$(mktemp -d "$rm_backup/recovered-output.XXXXXX") || exit 1
tar --acls --xattrs --selinux -xzpf "$rm_backup/monitor-output.tar.gz" -C "$rm_restore_dir" || exit 1
printf '과거 결과 복구 위치: %s\n' "$rm_restore_dir"
```

추가한 DB 인덱스는 1.8.0 조회에도 사용할 수 있으므로 패키지 복구 과정에서 자동 삭제하지 않습니다. DB 인덱스 제거가 필요하면 DBA가 별도로 판단합니다. DB 계정과 원본 수집 데이터는 패키지 교체·복구 대상이 아닙니다.
