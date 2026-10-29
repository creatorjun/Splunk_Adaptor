<!-- README.md -->
# Resource Monitor

현재 소스 버전은 **0.9.0**입니다. Oracle Linux 8/9용 C++23 데몬으로 CPU·메모리·마운트된 로컬 파일시스템과 역할별 Java `-DAPP` 프로세스를 수집합니다. 기본 60초마다 임계치 미만인 결과까지 모두 저장하고, 임계치 이상인 값의 `status`를 변경합니다. OS가 Asia/Seoul이면 일반 결과는 `/Monitor/20261029T123400+0900.json` 형식입니다.

`role`은 `col`(수집), `anal`(분석), `prx`(중계)입니다. 분석 서버는 `CloudESM.SUM_COLLECT_INFO`에서 완료된 5분 구간의 `count`만 장비별로 조회하고, 이를 시간별로 합산해 별도 `log_counts/log_cnt_<시각>.json`에 누적 보관합니다. 시간별 결과는 7일간 보관합니다. 종전 일별 증감 비교·무수신 감시·일일 집계는 제거했습니다. DB 테이블, 트리거, 인덱스를 생성하거나 변경하지 않습니다.

수집 서버는 처리 폴더의 `.tmp`·`.done` 개수가 임계치 이상인 항목을 기록합니다. 중계 서버는 `/CloudESM/logs/CollectorForAgent.log`의 ERROR 행 중 `connection`을 포함한 새 행을 기록합니다. 일반 설정은 `/etc/resource-monitor/settings.json`, DB 계정은 `/etc/resource-monitor/secrets/mysql.json`에서 관리하며 설정 변경은 재시작 없이 반영합니다.

설치 ZIP에는 Oracle Linux 8·9용 RPM, 역할별 샘플 설정, 이 README와 검증 기록을 함께 제공합니다. 대상 OS에 맞는 RPM을 선택하고 `docs/VALIDATION.md` 및 `docs/validation-ol8-0.9.0.json`·`docs/validation-ol9-0.9.0.json`에서 해당 배포본의 검사 결과를 확인하십시오. 0.9.0은 기존 1.x보다 버전 번호가 낮으므로 기존 장비는 아래 제거 후 설치 절차를 사용합니다.

1.8.0 설치 장비와 1.12.0 장비의 교체 방법은 [제거 후 0.9.0 재설치 가이드](#upgrade-from-1-8)를 참고하십시오. 기존 설정은 자동 변환하지 않으므로 새 버전 시작 전에 역할과 삭제된 설정 항목을 이전해야 합니다.

## 0.9.0 검토와 검증 범위

설정·수집·DB·저장·런타임·설치 구성을 검토하고, 관측 결과와 재시도·보존 규칙을 유지하면서 반복 작업을 줄였습니다.

- 유효한 `settings.json` 본문이 같으면 파싱 결과를 재사용합니다. 파일 읽기·종류·크기 검사는 계속 수행하며 수정 시각만으로 변경을 판단하지 않습니다. 제거된 `analyzer`, `log_silence`, `log_volume` 설정은 샘플에서 삭제했고 기존 장비는 이전 절차에서 제거합니다.
- DB 작업 스레드는 새 조회 구간이 요청될 때 깨우고, 한 시간의 누적 맵을 재사용하며 완료 보고서와 저장 대기 데이터를 이동해 불필요한 복사를 줄였습니다. 보존 처리 오류가 복구되면 빈 조회 구간의 `no_data` 상태도 올바르게 복원합니다.
- 마운트 수집은 한 번의 관측 안에서 같은 블록 장치·원본 경로 확인 결과를 재사용하고 마운트 경로 중복 확인에 해시 집합을 사용합니다. 다음 관측에서는 새로 확인하므로 장치 변경을 이전 캐시로 숨기지 않습니다.
- 중계 로그는 처음 30바이트로 ERROR 헤더가 아님을 확인한 행의 본문을 일찍 제외합니다. connection 필터, 미완성 행, 긴 행, 로그 회전 처리 규칙은 유지합니다.
- 보존 검사에서 크기가 제한된 JSON 읽기 버퍼를 미리 확보하며 파일 동기화와 원자 저장 절차는 유지합니다.
- RPM 설치 전 서비스 그룹·계정 생성이 실패하면 즉시 설치를 중단합니다. 실패 후 다음 단계로 계속 진행하던 오류 경로를 수정하고 실제 계정 생성 없이 셸 함수로 실패·성공 경로를 검사합니다.

배포본의 검증 기록에는 빌드에 사용한 소스 해시와 OS별 테스트·패키지 검사 결과를 명시합니다. 컨테이너에서 수행한 검사는 실제 장비의 systemd 부팅, SELinux 정책, 운영 DB 실행 계획까지 검증한 결과가 아닙니다. 설치 후 아래 절차에 따라 대상 장비에서 서비스 상태와 새 JSON 생성을 확인하십시오. 반복 작업을 줄인 변경의 실제 성능 개선량은 별도 측정이 필요합니다.

## 구성

| 위치 | 책임 |
| --- | --- |
| `src/domain` | 측정 모델, 계산식, 임계치와 상태, 5분·시간 집계 데이터 |
| `src/application` | 수집·설정 적용·프로세스 선택·집계·저장·보존 유스케이스와 포트 |
| `src/infrastructure/linux` | procfs, mountinfo, 파일시스템·Java 프로세스·처리 파일·중계 로그·현지 시간 구간 |
| `src/infrastructure/config` | 설정과 계정 JSON 파싱·검증 |
| `src/infrastructure/database` | 한 개의 비동기 MySQL 연결과 범위 집계 SELECT |
| `src/infrastructure/persistence` | 일반 JSON 원자 저장, 시간별 집계와 처리 위치 저장 |
| `src/runtime` | 수집·설정 확인 타이머와 종료 신호 |
| `src/main.cpp` | 구성 객체 생성과 의존성 주입 |
| `tests` | 단위·통합·의존성 방향·설치 검사 |
| `packaging` | systemd 단위, RPM 명세, Docker 빌드 환경 |

Domain은 Linux와 JSON을 참조하지 않습니다. Application은 자신의 포트와 Domain에만 의존합니다. 운영체제·DB·JSON은 외부 계층의 어댑터로 구현하며 테스트에서 의존성 방향을 검사합니다.

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

전체 설정 예시입니다. 역할과 DAPP 이름, DB 주소·TLS 경로는 장비에 맞게 변경하십시오.

```json
{
  "schema_version": 1,
  "role": "col",
  "process_monitor": {
    "anal": [
      "SearchEngine",
      "Normalizer",
      "CollectorForAgent",
      "CDMEngine",
      "ShovelServer",
      "AnalyzeEngine",
      "AnalyzeAssistantEngine",
      "SoarTicketMain",
      "SoarPlaybookManager",
      "pmsServer",
      "pmsClient",
      "ProcessMonitor"
    ],
    "col": [
      "SearchEngine",
      "Reducer",
      "Normalizer",
      "CloudESMCollector",
      "CollectorForAgent",
      "CDMEngine",
      "pmsClient",
      "ProcessMonitor"
    ],
    "prx": [
      "CollectorForAgent"
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
  "processing_files": {
    "thresholds": {
      "collector": {
        "tmp_count": 100,
        "done_count": 100
      },
      "normalize": {
        "tmp_count": 100,
        "done_count": 100
      }
    }
  },
  "thresholds": {
    "cpu_usage_percent": 80.0,
    "memory_usage_percent": 85.0,
    "storage_usage_percent": 90.0
  },
  "relay_log": {
    "path": "/CloudESM/logs/CollectorForAgent.log"
  },
  "mysql": {
    "host": "127.0.0.1",
    "port": 3306,
    "database": "CloudESM",
    "credentials_file": "/etc/resource-monitor/secrets/mysql.json",
    "connect_timeout_seconds": 3,
    "read_timeout_seconds": 3,
    "write_timeout_seconds": 3,
    "tls_mode": "verify_identity",
    "tls_ca_file": "/etc/resource-monitor/secrets/mysql-ca.pem"
  }
}
```

| 설정 | 허용 값과 기본값 |
| --- | --- |
| `role` | `col`, `anal`, `prx`; 기본 `col` |
| `process_monitor.col` | 수집 서버 DAPP 배열 |
| `process_monitor.anal` | 분석 서버 DAPP 배열 |
| `process_monitor.prx` | 중계 서버 DAPP 배열; 기본 `["CollectorForAgent"]` |
| `relay_log.path` | 절대 일반 파일 경로; 기본 `/CloudESM/logs/CollectorForAgent.log` |
| `collection.interval_seconds` | 리소스 JSON 저장 주기, 1~86400초; 기본 60초 |
| `collection.output_directory` | 미리 준비한 쓰기 가능한 절대 디렉터리; 기본 `/Monitor` |
| `retention.enabled` | 일반 리소스 JSON 삭제 여부; 기본 `true` |
| `retention.max_age_seconds` | 일반 리소스 JSON 보존 시간, 60~31536000초; 기본 3600초 |
| `thresholds.cpu_usage_percent` | 0~100의 유한한 숫자; 필수 |
| `thresholds.memory_usage_percent` | 0~100의 유한한 숫자; 필수 |
| `thresholds.storage_usage_percent` | 0~100의 유한한 숫자; 필수 |
| `processing_files.thresholds.collector.tmp_count` | 0~1,000,000,000,000 정수; 기본 100개 |
| `processing_files.thresholds.collector.done_count` | 같은 범위; 기본 100개 |
| `processing_files.thresholds.normalize.tmp_count` | 같은 범위; 기본 100개 |
| `processing_files.thresholds.normalize.done_count` | 같은 범위; 기본 100개 |

로그 집계 주기 5분, 출력 단위 1시간, 시간별 파일 보존 7일은 고정입니다. `collection.interval_seconds`와 `retention`은 일반 리소스 JSON 설정이며 이 집계 주기·보존 기간을 변경하지 않습니다. 새 설정에는 `analyzer`, `log_silence`, `log_volume`, `is_analyzer`, `mysql.enabled`를 넣지 않습니다. `role`의 이전 값 `collector`, `analyzer`, `relay`와 `process_monitor`의 이전 이름도 허용하지 않습니다.

설정은 최대 64KiB의 일반 JSON 파일이어야 합니다. 중복·알 수 없는 키, 잘못된 자료형, null, 심볼릭 링크와 FIFO는 거부합니다. 생략 가능한 객체는 기본값을 사용하지만 세 리소스 임계치는 필수입니다. 임계치 0은 정상 측정한 값에 항상 경고를 발생시키며 정확히 임계치인 값도 경고입니다. 결과 JSON에는 임계값 숫자를 복제하지 않습니다.

데몬은 수집 타이머와 별도로 약 1초마다 설정을 확인합니다. 유효한 변경은 자동 반영하며 잘못된 문서는 기존 유효 설정을 유지하고 `config_status=using_last_valid`와 오류를 기록합니다. 시작부터 잘못된 문서는 종료 코드 78로 실패하며 systemd는 이 코드로 반복 재시작하지 않습니다. `--check-config`는 문법·값과 분석 역할의 계정 파일만 검사하며 실제 DB 접속·TLS·저장 성공을 보증하지 않습니다.

```bash
runuser -u resource-monitor -- /usr/sbin/resource-monitor --config /etc/resource-monitor/settings.json --check-config
```

수집 주기 변경은 적용 시점부터 새 주기를 시작합니다. CPU 차분은 직전 실제 샘플부터 계산하므로 첫 전환 구간 길이가 새 주기와 다를 수 있습니다. 저장 경로만 바꾸면 수집 타이머는 유지합니다. 명령줄 `--interval-seconds`·`--output`은 해당 JSON 값보다 우선합니다. 기본 서비스는 이 옵션들을 지정하지 않습니다.

출력 경로는 서비스 계정이 읽고 쓰고 탐색할 수 있는 로컬 디렉터리로 먼저 준비합니다. 데몬은 임의의 출력 루트를 자동 생성하지 않습니다. 경로 전환 시 디렉터리 잠금과 실제 임시파일 저장을 검사하고 실패하면 이전 경로를 유지합니다. 별도 마운트라면 systemd의 `RequiresMountsFor`도 실제 경로에 맞추고 ACL·SELinux를 확인하십시오. `log_counts` 하위 폴더는 분석 기능이 준비한 출력 루트 아래에 생성합니다.

| `role` | 리소스·DAPP | DB 5분 집계 | 처리 폴더 | 중계 connection ERROR 로그 |
| --- | --- | --- | --- | --- |
| `col` | 사용 | 사용 안 함 | 사용 | 사용 안 함 |
| `anal` | 사용 | 사용 | 사용 안 함 | 사용 안 함 |
| `prx` | 사용 | 사용 안 함 | 사용 안 함 | 사용 |

## MySQL 연결 설정

`role="anal"`일 때만 계정 파일을 읽고 DB에 접속합니다. 이 역할에서는 `mysql` 객체와 `mysql.database`가 필수입니다. `col`·`prx`는 계정 파일을 읽거나 DB 조회를 시작하지 않습니다. 접속 주소와 동작 설정은 `settings.json`, 사용자명·비밀번호는 별도 계정 파일에 저장합니다.

| 설정 | 의미와 기본값 |
| --- | --- |
| `mysql.host` | 기본 `127.0.0.1` |
| `mysql.port` | 1~65535, 기본 3306; 실제 수신 포트 확인 |
| `mysql.database` | 분석 역할에서 필수; 대상 테이블은 `CloudESM.SUM_COLLECT_INFO`로 고정 |
| `mysql.credentials_file` | 기본 `/etc/resource-monitor/secrets/mysql.json` |
| `mysql.connect_timeout_seconds` | 1~10초, 기본 3초 |
| `mysql.read_timeout_seconds` | 1~10초, 기본 3초 |
| `mysql.write_timeout_seconds` | 1~10초, 기본 3초 |
| `mysql.tls_mode` | `verify_identity` 또는 `disabled`; 기본 `verify_identity` |
| `mysql.tls_ca_file` | 분석 역할의 `verify_identity`에서 필수인 신뢰 CA 절대 경로 |

`verify_identity`는 암호화와 서버 인증서·호스트명 검증을 요구합니다. IP로 접속하면 인증서에 해당 IP SAN이 있어야 합니다. TLS를 제공하지 않는 신뢰된 로컬 시험 DB에서는 `disabled`를 명시할 수 있습니다. 계정 파일은 다음 형식이며 실제 비밀번호를 명령줄에 직접 넣지 않습니다.

```json
{
  "username": "replace-with-database-user",
  "password": "replace-with-database-password"
}
```

```bash
sudo install -d -m 0750 -o root -g resource-monitor /etc/resource-monitor/secrets
if ! sudo test -e /etc/resource-monitor/secrets/mysql.json && ! sudo test -L /etc/resource-monitor/secrets/mysql.json; then
    sudo install -m 0640 -o root -g resource-monitor /usr/share/resource-monitor/mysql.example.json /etc/resource-monitor/secrets/mysql.json
fi
sudoedit /etc/resource-monitor/secrets/mysql.json
sudo chown root:resource-monitor /etc/resource-monitor/secrets/mysql.json
sudo chmod 0640 /etc/resource-monitor/secrets/mysql.json
```

계정 파일은 root 또는 실행 계정 소유의 최대 16KiB 일반 파일이어야 하고 0600 또는 0640 권한만 허용합니다. 심볼릭 링크와 FIFO는 허용하지 않습니다. 사용자명은 1~128바이트이며 제어문자를 허용하지 않고 비밀번호는 NUL 없는 1~4096바이트입니다. 실제 계정 파일은 Git과 설치 ZIP에 넣지 않습니다.

계정 또는 DB 접속 설정이 바뀌면 새 연결과 `SELECT 1` 확인 후 연결을 전환합니다. 연결 실패 중에도 일반 설정 변경과 리소스 수집은 계속합니다. 연결 재시도는 5초부터 최대 60초까지 늘어나며 오류를 `connection_pending`으로 숨기지 않습니다. 정상 연결은 30초마다 `SELECT 1`로 확인합니다. 접속·조회는 하나의 별도 작업 스레드에서 실행하므로 응답을 기다리느라 리소스 수집을 멈추지 않습니다.

DB 쿼리는 연결 확인용 `SELECT 1`과 아래 범위 집계 SELECT뿐입니다. 데몬은 기존 DB의 테이블·인덱스·트리거를 변경하지 않습니다. 조회 계정에는 `CloudESM.SUM_COLLECT_INFO`의 SELECT 권한만 필요합니다. 기존 계정이 있다면 비밀번호나 계정을 다시 만들 필요가 없습니다. 계정 권한을 준비하는 DBA용 예시는 다음과 같습니다.

```sql
GRANT SELECT ON CloudESM.SUM_COLLECT_INFO TO 'resource_monitor'@'127.0.0.1';
SHOW GRANTS FOR 'resource_monitor'@'127.0.0.1';
```

위 SQL은 이미 생성한 계정을 전제로 합니다. 권한은 즉시 반영되며 별도 FLUSH 명령은 필요하지 않습니다. Linux 셸에서 실제 포트로 접속을 검증합니다.

```bash
mysql --no-defaults --protocol=TCP -h 127.0.0.1 -P 13306 -u resource_monitor -p CloudESM
```

## 5분 증분 조회와 시간별 로그 건수

`SUM_COLLECT_INFO.recv_time`은 `YYYYMMDDHHmm` 형식의 분 단위 BIGINT이고 `count`는 해당 행의 수집 건수입니다. 모든 과거 행을 프로그램으로 가져오지 않고, 완료된 5분의 시작 포함·끝 제외 범위를 DB에서 `eqp_ip`별로 합산합니다. 같은 IP의 여러 로그 분류와 여러 행을 합산합니다. 행의 개수가 아니라 `SUM(count)`가 로그 건수입니다.

예를 들어 13:05에 조회할 구간은 13:00:00 이상, 13:05:00 미만입니다. 다음 조회는 13:05 이상, 13:10 미만으로 이어지므로 이미 성공 처리한 구간을 매번 다시 읽지 않습니다. SQL의 집계 보조값은 NULL·음수·잘못된 합계를 검증하기 위한 값입니다.

```sql
SELECT eqp_ip, SUM(`count`), COUNT(*), COUNT(`count`), MIN(`count`),
       recv_time - MOD(recv_time, 5) AS interval_start
FROM CloudESM.SUM_COLLECT_INFO
WHERE recv_time >= 202610291300
  AND recv_time < 202610291305
GROUP BY eqp_ip, interval_start
ORDER BY interval_start, eqp_ip
LIMIT 120001;
```

장비 IP는 해당 구간의 DB 결과에서 가져오며 하드코딩하지 않습니다. NULL 건수·음수·비어 있거나 39바이트를 넘거나 제어문자가 있는 장비 식별값·오버플로·장비 한도 초과는 정상 0건으로 바꾸지 않고 구간 오류로 처리합니다. 빈 구간은 정상 빈 결과로 처리하며 없던 장비를 임의로 0건 행으로 만들지 않습니다. 시간 조건에 함수를 씌우지 않으므로 기존 `recv_time` 선두 인덱스를 활용할 수 있습니다. 인덱스가 없으면 짧은 범위 SELECT도 DB 내부 전체 탐색이 될 수 있으므로 실제 실행 계획과 기존 인덱스 상태를 DBA가 확인해야 합니다. 데몬은 DDL을 실행하지 않습니다.

시간별 파일은 다음 구조로 저장합니다. 파일명은 `log_cnt_` 뒤에 집계 시간의 시작 시각과 UTC 오프셋을 붙입니다.

```text
/Monitor/
  20261029T140000+0900.json
  log_counts/
    .checkpoint.json
    log_cnt_20261029T130000+0900.json
    log_cnt_20261029T140000+0900.json
```

정상 연속 실행 시 13:05~14:00에 완료되는 12개 5분 구간을 합산해 14:00 이후 `log_counts/log_cnt_20261029T130000+0900.json`을 원자 저장합니다. 실제 저장은 마지막 조회·파일 동기화가 끝난 시점이므로 정각보다 늦을 수 있습니다. CPU·메모리·디스크를 담는 타임스탬프 JSON에는 장비별 건수를 반복 복사하지 않고 집계 상태만 기록합니다.

체크포인트가 없는 첫 실행은 가장 최근에 완료된 5분 구간부터 시작합니다. 따라서 첫 시간은 전체 1시간이 아닐 수 있으며 `coverage.complete=false`, `coverage.start`, `coverage.completed_intervals`로 실제 집계 범위를 표시합니다. 이후 연속 실행한 완전한 시간은 12개 구간을 포함하고 `coverage.complete=true`입니다.

| 시간별 파일 항목 | 의미 |
| --- | --- |
| `kind` | `resource-monitor-log-counts` |
| `source` | `CloudESM.SUM_COLLECT_INFO` |
| `window.start`, `window.end` | 한 시간의 시작 포함·끝 제외 범위와 OS 시간대 |
| `window.start_epoch`, `window.end_epoch` | 같은 경계를 나타내는 Unix 초 |
| `coverage.start` | 이번 시간에서 실제 집계를 시작한 시각 |
| `coverage.completed_intervals` | 성공 처리한 5분 구간 수 |
| `coverage.expected_intervals` | 완전한 시간의 구간 수 12 |
| `coverage.complete` | 전체 1시간을 12개 구간 모두 집계했는지 여부 |
| `equipment` | `{eqp_ip, count}` 목록; 해당 시간의 구간별 건수 합계 |
| `collected_at` | 최종 조회·집계를 완료한 시각 |
| `source_identity` | 접속 대상과 계정 파일 경로를 구분하는 내부 식별값; 비밀번호는 포함하지 않음 |

성공한 구간의 건수와 다음 조회 시작 위치는 `log_counts/.checkpoint.json`에 함께 저장합니다. DB 조회 또는 파일 저장에 실패하면 처리 위치를 전진시키지 않습니다. 재시작 시 이 파일을 읽어 이어서 처리하며, 저장 실패 후 재시도는 동일 시간 파일에 합계를 덧붙이지 않고 확정 합계로 원자 교체하므로 같은 구간을 두 번 더하지 않습니다. 시간별 결과를 저장한 후 체크포인트를 확정하며 충돌하는 다른 접속 대상의 파일은 덮어쓰지 않고 오류를 냅니다. 체크포인트를 임의로 편집·삭제하거나 실행 중 잠금 파일을 바꾸지 마십시오.

중단된 구간은 체크포인트 이후부터 다시 가져옵니다. 복구 중에도 집계 SELECT는 5분에 한 번만 실행하며 한 번에 최대 1시간을 5분 단위로 묶어 조회합니다. `log_volume.backlog=true`이면 아직 따라잡을 구간이 남은 것입니다. DB 접속이 끊겼거나 쿼리가 실패하면 재시도에서도 마지막 실패 원인을 유지하고 일반 리소스 JSON은 계속 생성합니다. 누락 구간을 실패한 0건으로 합산하지 않습니다.

이 증분 방식은 **성공 처리한 수신 시간 구간을 재조회하지 않는 방식**입니다. 기존 테이블에는 행 변경 번호가 없으므로 이미 처리한 과거 `recv_time` 행이 나중에 추가되거나 `count`가 수정돼도 자동 보정하지 않습니다. 원본의 집계 완료 시점과 조회 시점이 맞아야 합니다. 날짜·시간을 DB가 저장하는 OS 시간대와 데몬 시간대도 일치시켜야 합니다. 일광 절약 시간으로 분 단위 현지 시각을 유일하게 표현할 수 없는 구간은 정상 집계로 처리하지 않고 오류로 표시합니다.

출력 경로를 변경하면 누적 상태를 새 경로로 옮겨 이어가고 이전 경로의 완료 파일은 이동하거나 삭제하지 않습니다. DB 호스트·포트·데이터베이스·계정 파일 경로 변경은 다른 데이터 원본으로 구분합니다. 다른 원본으로 전환할 때에는 기존 결과와 같은 시간 파일이 충돌하지 않도록 새 출력 경로를 사용하는 것이 좋습니다. 동일 계정 파일의 비밀번호 변경은 기존 집계 진행 위치를 유지합니다.

시간별 결과는 **집계 종료 시각부터 7일 이상** 지난 파일을 약 5분마다 정리합니다. 파일명과 본문의 집계 시각·종류를 검증한 `log_cnt_<시각>.json` 일반 파일만 삭제합니다. 체크포인트·잠금·임시파일·관련 없는 JSON·심볼릭 링크와 하위 디렉터리는 삭제하지 않습니다. 일반 리소스 JSON의 `retention.enabled=false`여도 시간별 파일의 7일 보존은 적용됩니다. 분석 기능이 비활성화되거나 출력 경로를 변경한 뒤에는 이전 경로를 계속 정리하지 않습니다.

## Java 프로세스 모니터링

`role`에 따라 `process_monitor.anal`, `process_monitor.col`, `process_monitor.prx` 중 해당 목록을 선택하여 매 수집 주기에 Java 프로세스를 관측합니다. 대상은 `-DAPP=<이름>`을 기준으로 하며 대소문자를 구분합니다. 다음은 각 목록을 생략했을 때 사용하는 기본값입니다.

| `role` | `process_profile` | 대상 DAPP 이름 |
| --- | --- | --- |
| `anal` | `anal` | `SearchEngine`, `Normalizer`, `CollectorForAgent`, `CDMEngine`, `ShovelServer`, `AnalyzeEngine`, `AnalyzeAssistantEngine`, `SoarTicketMain`, `SoarPlaybookManager`, `pmsServer`, `pmsClient`, `ProcessMonitor` |
| `col` | `col` | `SearchEngine`, `Reducer`, `Normalizer`, `CloudESMCollector`, `CollectorForAgent`, `CDMEngine`, `pmsClient`, `ProcessMonitor` |
| `prx` | `prx` | `CollectorForAgent` |

세 목록은 각각 0~256개의 문자열을 허용합니다. 이름은 UTF-8 기준 1~128바이트이며 제어문자와 앞뒤 ASCII 공백을 허용하지 않습니다. 이름 내부의 공백은 허용하며 JVM 인자 값 전체와 정확히 비교합니다. 같은 목록에서 동일한 이름을 중복 지정할 수 없으며 대소문자가 다른 이름은 서로 다른 대상입니다. 현재 선택되지 않은 역할의 목록도 함께 검증합니다. `process_monitor` 객체 전체를 생략하거나 일부 목록을 생략하면 해당 목록에 기존 기본값을 사용합니다. 빈 배열 `[]`을 명시하면 해당 역할의 프로세스 관측을 생략하고 `processes={}`, `process_status=ok`를 기록합니다. 이 경우에도 리소스 수집과 `role`에 따른 DB·처리 파일·중계 로그 동작은 계속합니다.

`/proc`에서 Java 프로세스의 인자를 확인하며 `ps`나 `grep` 명령을 실행하지 않습니다. `-DAPP`가 없는 Tomcat·메신저 등의 Java 프로세스와 `grep java`는 대상에 포함하지 않습니다. 이름의 일부가 일치하는 것만으로는 실행 중으로 판정하지 않습니다. 같은 DAPP 이름으로 여러 프로세스가 실행 중이면 살아 있는 인스턴스가 하나라도 있을 때 `true`입니다.

`processes`는 선택한 대상 이름을 모두 포함합니다. 실행 중인 대상은 `true`, 관측을 완료했으나 해당 프로세스가 없거나 종료·좀비 상태이면 `false`입니다. 실행 여부를 확인하는 기능이므로 `true`가 애플리케이션의 응답 정상이나 처리 정상까지 보장하지는 않습니다. 대상의 미실행을 확인하면 `process_status=warning`이며 전체 `status`에도 경고를 반영합니다. 누락된 프로세스를 자동으로 시작하거나 재시작하지 않습니다.

프로세스 목록이나 인자를 권한 문제 등으로 충분히 확인하지 못하면 발견된 실행 중 대상은 `true`로 유지하고, 존재 여부를 확정할 수 없는 대상은 `null`로 기록합니다. 이 경우 `process_status=unknown`과 `errors`에 관측 오류를 남깁니다. 서비스 계정이 호스트의 프로세스를 볼 수 있도록 `/proc` 접근 권한을 준비해야 합니다. 프로세스 조회도 순차적으로 수행하므로 저장 순간과 완전히 동일한 시점의 원자적 상태는 아닙니다.

다음은 수집 장비에서 `Reducer`가 미실행인 경우의 JSON 일부입니다. 값은 문자열이 아닌 JSON 불리언입니다.

```json
{
  "status": "warning",
  "role": "col",
  "process_profile": "col",
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

`settings.json`의 `role` 또는 `process_monitor` 목록을 수정하면 1초마다 수행하는 설정 확인을 거쳐 재시작 없이 다음 수집부터 적용합니다. 목록에 추가한 이름은 다음 결과에 나타나고 삭제한 이름은 결과에서 사라집니다. 목록만 변경하면 DB를 재연결하지 않습니다. 중복 이름이나 문자열이 아닌 항목 등으로 적용에 실패하면 기존 설정 전체와 대상 목록을 유지하고 `config_status=using_last_valid`를 기록합니다. 올바른 파일로 수정하면 자동으로 복구합니다. 프로세스 관측은 DB 쿼리 결과에 의존하지 않으며 DB 분석 중단이나 조회 실패 중에도 계속합니다. 수집 주기와 저장 경로는 기존 `collection` 설정을 함께 사용합니다.

## 수집 장비 처리 파일 개수 모니터링

`role="col"`일 때만 `/CloudESM/data/processing/collector`와 `/CloudESM/data/processing/normalize`를 하위 폴더까지 검사합니다. 각 루트 폴더에서 일반 파일의 `.tmp`와 `.done` 개수를 따로 합산하므로 총 네 개의 임계치를 적용합니다. 하위 폴더별 목록이나 파일 내용은 기록하지 않습니다. 대소문자를 구분하며 숨김 파일도 포함합니다. 이름이 `.tmp`로 끝나더라도 디렉터리·FIFO·심볼릭 링크는 세지 않습니다. 하위 심볼릭 링크 디렉터리도 따라가지 않습니다. `/CloudESM/data` 같은 상위 경로의 링크는 허용하지만 검사 대상 루트인 `collector`·`normalize` 자체가 링크이면 오류로 처리합니다.

기본 임계치는 모두 100개입니다. 예를 들어 collector의 `.tmp`가 99개면 해당 항목을 생략하고, 100개부터 출력합니다. 한 항목이라도 임계치 이상이면 `processing_files.alerts`에 그 항목만 추가하고 최상위 `status`를 `warning`으로 반영합니다. 네 항목 모두 임계치 미만이거나 역할이 `col`가 아니면 `processing_files` 객체 자체를 생략합니다. CPU·메모리·디스크 등 기존 결과 파일은 매 수집 주기마다 계속 생성됩니다. 임계치 0은 명시적으로 허용하며, 정상 검사한 개수가 0이어도 해당 항목을 기록합니다.

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

설정 파일의 임계치 변경은 재시작 없이 기존의 유효한 검사 결과에 적용합니다. 역할을 `anal` 또는 `prx`로 바꾸면 진행 중인 검사에 취소를 요청하고 이전 결과를 버리며, 수집 서버로 돌아오면 새 검사를 시작합니다. 검사 준비 중이거나 완료 후 120초 넘게 오래된 결과는 경고 개수로 재사용하지 않습니다. 파일 작업이 커널에서 지연되면 취소 처리는 해당 호출이 반환된 뒤 진행됩니다.

없는 폴더, 접근 권한 오류, 검사 제한 초과는 0개로 처리하지 않습니다. 오류가 발생한 루트의 개수는 경고 판정에 사용하지 않고 최상위 `errors`에 `component=processing_files`로 원인을 기록합니다. 다른 루트를 정상 검사했다면 그 결과는 사용합니다. 오류 외에 경고가 없으면 최상위 상태는 `unknown`이며 리소스 전용 `collection_status`에는 영향을 주지 않습니다. 과도한 탐색을 막기 위해 하위 깊이 128, 루트당 1천만 항목, 전체 검사 시간 55초의 제한을 적용합니다. 데몬은 감시 폴더나 파일을 생성·수정·삭제하지 않습니다. 서비스 계정에 대상 폴더와 하위 폴더의 읽기·탐색 권한을 제공해야 합니다.

## 중계 서버의 CollectorForAgent 오류 로그

`role="prx"`일 때만 `relay_log.path`의 로그를 읽습니다. 기본 경로는 `/CloudESM/logs/CollectorForAgent.log`이며 일반 리소스 수집 주기마다 검사합니다. 기본 수집 주기 60초에서는 다음 주기의 JSON에 새 오류를 기록합니다. 이 기능은 결과 JSON 기록이며 별도 메일이나 외부 알림을 전송하지 않습니다. DB 연결·조회와 수집 서버 처리 폴더 검사는 중계 역할에서 실행하지 않습니다.

기존 settings에서 역할과 로그 경로는 다음처럼 지정합니다. 이전 `is_analyzer`, `analyzer`, `log_silence`, `log_volume` 키를 제거하고 역할별 `process_monitor` 키도 `col`·`anal`·`prx`로 변경합니다. 상세 변환은 아래 이전 가이드에 있습니다. 리소스·저장·DB 접속 등 나머지 설정은 유지합니다.

```json
{
  "role": "prx",
  "relay_log": {
    "path": "/CloudESM/logs/CollectorForAgent.log"
  },
  "process_monitor": {
    "prx": ["CollectorForAgent"]
  }
}
```

이 예제는 전체 설정이 아닌 추가·변경할 항목입니다. `role`, `relay_log.path`, `process_monitor.prx` 수정은 데몬 재시작 없이 반영합니다. DAPP 목록은 해당 장비의 실제 `-DAPP=` 이름으로 맞춥니다.

`2026-10-29 10:42:07.413 ERROR [HTTP post thread-1] ... - Http post exception ...` 형식에서 날짜·시각과 정확한 `ERROR` 레벨 토큰을 확인하고, 그 행에 `connection`이 대소문자 구분 없이 포함된 경우만 기록합니다. `Connection`, `CONNECTION`, `ConnectException: Connection refused`는 일치합니다. ERROR여도 connection이 없는 행은 제외하고, INFO·WARN의 connection 문구도 제외합니다. 레벨 `ERROR` 자체는 대소문자를 구분합니다. JSON에는 원문의 시각 문자열, 첫 ` - ` 이후 본문, 개행을 제외한 한 줄 원문을 함께 기록하며 후속 Java 스택 트레이스 행은 이번 검사에 포함하지 않습니다. 원문 시각에는 시간대가 없으므로 임의의 오프셋을 붙이지 않고, 데몬의 검사 시각 `checked_at`에는 OS 시간대를 적용합니다.

결과 JSON의 해당 부분은 다음과 같습니다.

```json
{
  "role": "prx",
  "relay_log": {
    "status": "warning",
    "source": "/CloudESM/logs/CollectorForAgent.log",
    "checked_at": "2026-10-29T10:43:00.000+09:00",
    "error_count": 1,
    "events": [
      {
        "timestamp": "2026-10-29 10:42:07.413",
        "message": "Http post exception on https://192.0.2.1:443/: java.net.ConnectException: Connection refused (Connection refused)",
        "line": "2026-10-29 10:42:07.413 ERROR [HTTP post thread-1] c.s.c.s.HttpLogSender.reliablySend:425 - Http post exception on https://192.0.2.1:443/: java.net.ConnectException: Connection refused (Connection refused)",
        "truncated": false
      }
    ],
    "backlog": false,
    "errors": []
  }
}
```

`error_count`는 해당 JSON에 담긴 connection 관련 ERROR 행 수입니다. 같은 본문이 재시도로 반복 출력되면 서로 다른 새 행으로 기록합니다. ERROR가 있으면 `relay_log.status=warning`이며 전체 `status`도 `warning`입니다. 새 ERROR가 없고 읽기에 성공하면 `ok`와 빈 `events`를 기록합니다. 이는 그 검사 구간에 새 오류가 없다는 뜻이며 원래 장애가 해결됐다는 판정은 아닙니다. 파일 접근·읽기 실패는 `relay_log.errors`와 최상위 `errors`에 기록하고 ERROR 행과 구분합니다. ERROR 행 없이 읽기 오류가 있으면 `error`, 읽지 못한 데이터나 줄바꿈 대기만 남으면 `partial`, 중계 역할이 아니면 `disabled`입니다. `error`·`partial`은 다른 경고가 없으면 전체 `status=unknown`으로 반영하며 CPU·메모리·디스크 수집을 중단하지 않습니다.

첫 활성화·데몬 재시작·경로 변경 시 이미 존재하는 파일은 끝에서 시작하므로 과거 오류를 재전송하지 않습니다. 최초 확인 당시 파일이 없었다가 생성되면 새 파일의 처음부터 읽습니다. JSON 저장이 성공한 뒤에만 보관한 오류를 비우므로 저장 실패 중 읽은 오류는 다음 저장에서 재시도합니다. 이 대기열과 읽기 위치는 메모리에만 있으며 재시작, 역할 비활성화, 경로 변경 시 초기화됩니다. 저장이 실패로 보고됐지만 일부 결과 파일이 이미 남은 경우 재시도로 중복 기록될 수 있으므로 정확히 한 번 전달하는 보장은 아닙니다.

한 번에 최대 1MiB를 읽고, 확인되지 않은 ERROR는 최대 100건, 한 줄은 최대 16KiB를 보관합니다. 줄이 더 길면 앞부분과 `truncated=true`를 기록합니다. 유효하지 않은 UTF-8 바이트는 JSON 저장 시 대체 문자로 바꿉니다. 아직 읽을 내용이나 미완성 행이 남으면 `backlog=true`로 표시하고 다음 검사에서 이어 읽습니다. 검사 중 발견한 로그 회전은 이전 열린 파일의 남은 내용을 먼저 읽고 새 파일의 처음으로 이동합니다. 동시에 보관하는 파일은 최대 8개이며 초과는 오류로 표시합니다. 잘림으로 미처리 데이터나 미완성 행을 잃은 것이 확인되면 오류를 남깁니다. 검사 사이에 일어난 여러 번의 회전, 이전 파일의 EOF 확인 후 추가 기록, copytruncate 후 기존 위치 이상으로 재증가한 파일의 모든 행을 보장하지는 않습니다. 손실 없는 로그 전달이 필요하면 별도 로그 수집기의 영속 체크포인트를 사용해야 합니다.

로그는 일반 파일이어야 하고 최종 경로의 심볼릭 링크·FIFO·디렉터리는 거부합니다. `resource-monitor` 서비스 계정에 상위 디렉터리 탐색 권한과 파일 읽기 권한이 필요하며 로그 회전으로 생성되는 새 파일에도 유지되어야 합니다. 실제 권한을 먼저 확인하고 운영 그룹 또는 ACL로 필요한 접근만 부여합니다.

```bash
namei -l /CloudESM/logs/CollectorForAgent.log
runuser -u resource-monitor -- test -r /CloudESM/logs/CollectorForAgent.log
journalctl -u resource-monitor --since '-10 minutes' --no-pager
```

## 일반 리소스 JSON 계약

| 필드 | 의미 |
| --- | --- |
| `schema_version` | 1 |
| `timestamp` | OS 현지 시각과 UTC 오프셋 |
| `role`, `process_profile` | 현재 `col`, `anal`, `prx` |
| `status` | 전체 `ok`, `warning`, `unknown` |
| `collection_status` | CPU·메모리·스토리지 수집 상태 |
| `config_status` | `ok` 또는 `using_last_valid` |
| `thresholds_applied_at` | 임계치 설정을 적용한 시각; 임계값 숫자는 출력하지 않음 |
| `cpu`, `memory`, `filesystems` | 사용량·사용률·상태 |
| `processes`, `process_status` | DAPP 이름별 true/false/null과 전체 프로세스 상태 |
| `processing_files` | 수집 역할의 임계치 이상 파일 개수; 해당 항목 없으면 생략 |
| `relay_log` | 중계 로그의 새 ERROR와 읽기 오류 |
| `log_volume` | 분석 역할의 5분 증분·시간 합계 처리 상태와 오류 |
| `errors` | 이번 결과에서 확인한 오류 |

리소스·DAPP·처리 파일·중계 로그에서 경고가 하나라도 있으면 `warning`, 경고 없이 측정 불가나 집계 오류가 있으면 `unknown`, 모두 정상 측정됐으면 `ok`입니다. 역할상 비활성 기능은 전체 상태에 영향을 주지 않습니다. DB 오류는 CPU·메모리·스토리지 개별 값과 `collection_status`를 변경하지 않습니다. `collect_count`와 `log_silence` 출력은 제거되었으며 과거 JSON을 소급 수정하지 않습니다. 외부 소비자는 새 역할 값과 삭제된 필드를 반영해야 합니다.

`log_volume.last_saved_hour`는 마지막 저장 완료 시간의 시작 시각, `completed_intervals`는 현재 누적 중인 미완료 시간의 처리 구간 수, `backlog`는 다음 조회해야 할 완료 구간이 남아 있는지 나타냅니다. 빈 조회 구간의 `no_data`는 오류나 경고를 만들지는 않지만 다른 경고가 없으면 전체 상태를 `unknown`으로 표시합니다.

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

시작 시 CPU 기준값을 확보하고 기본 60초 후 정기 수집과 임계치 판정을 수행하여 전체 결과를 저장합니다. 이후 매 수집 주기에 리소스·Java 프로세스 수집, CPU 기준값 갱신, 임계치 판정과 JSON 저장을 수행합니다. 5분 집계 처리 상태가 갱신되거나 새 오류가 발생하면 이 정기 일정 외에도 전체 결과를 추가 저장하므로 첫 파일이 60초보다 일찍 생성될 수 있습니다. 경고에서 정상으로 복구된 주기와 측정할 수 없는 항목이 있는 주기도 같은 JSON 형식으로 기록합니다. 리소스 정기 수집은 Linux CLOCK_MONOTONIC 타이머를 사용하므로 OS 시간대나 UTC/NTP 시간 변경이 주기를 바꾸지 않습니다. 구간 수집량은 달력 시각을 다시 계산하므로 시각 변경을 반영합니다. 작업 중 지나간 주기는 건너뛰고 다음 주기를 기다립니다. 실제 CPU 관측 구간은 `cpu.window_seconds`에 기록됩니다. 스케줄러나 파일 I/O 지연에 따라 설정한 주기보다 길어질 수 있으므로 설정 주기를 실제 관측 시간의 엄격한 상한으로 해석하지 않습니다.

저장할 때마다 `tzset()`으로 시스템 시간대 정보를 갱신하고 JSON 시각과 파일명에 같은 시간대를 적용합니다. 기본 systemd 서비스처럼 `TZ` 환경 변수를 별도로 지정하지 않은 경우 `/etc/localtime`을 따르며, OS 시간대 변경은 데몬 재시작 없이 다음 저장부터 반영됩니다. 별도로 `TZ`를 지정해 실행하면 해당 프로세스의 시간대 설정이 우선합니다. 기존 수집 시각과 임계치 적용 시각의 실제 시점은 유지하지만, 5분 집계의 현지 날짜·DB 조회 구간은 변경된 현지 시간대를 사용합니다.

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

삭제 범위는 현재 적용된 `collection.output_directory`(기본 `/Monitor`)의 직계 파일입니다. 분석·수집·중계 역할에 관계없이 같은 규칙을 적용합니다. 출력 경로를 변경하면 다음 정리부터 새 경로만 검사하며 이전 경로의 남은 파일을 계속 삭제하지 않습니다. 정리 간격은 60초로 고정되어 있으며 수집 주기를 더 길게 바꾸어도 유지됩니다. 실행 중 작업이나 대량 과거 파일 검사에 시간이 걸리면 실제 정리 완료가 늦어질 수 있습니다.

`YYYYMMDDTHHMMSS+HHMM.json`, 음수 오프셋, 기존 `Z` 형식과 `_1`~`_9999` 충돌 접미사를 인식합니다. 유효한 날짜와 모니터링 JSON 필수 구조를 확인한 16MiB 이하의 일반 파일만 삭제합니다. 다른 JSON, 내용이 손상되거나 시각이 일치하지 않는 파일, 미래 시각 파일, 하위 폴더, 심볼릭 링크, 하드링크, 잠금 파일과 `.tmp`는 보존합니다. `/CloudESM/data/processing/collector`와 `normalize`의 `.tmp`·`.done`은 삭제하지 않습니다.

삭제 성공 시 journal에 `snapshots_pruned count=N`, 실패 시 `component=snapshot_retention` 오류를 남깁니다. 삭제 실패는 리소스 수집과 새 JSON 저장을 중단시키지 않으며 다음 정리 시 다시 시도합니다. 이 유지관리 결과는 현재 JSON 스키마에 새 필드를 추가하지 않고 journal에 기록합니다.

```bash
journalctl -u resource-monitor --since '-10 minutes' --no-pager
```

기본 60초 정기 수집에서는 하루 약 1,440개 파일을 생성하고, 분석 서버는 집계 상태 갱신에 따른 파일이 추가될 수 있습니다. 기본 보존 설정에서는 통상 최근 1시간 분량을 유지합니다. 보존 시간을 늘리거나 자동 삭제를 끄면 그만큼 파일이 누적됩니다. 업그레이드 후 첫 실행부터 이미 존재하는 정상 모니터링 JSON에도 현재 보존 설정이 적용되므로, 보관 요구에 맞는 값을 시작 전에 지정하십시오.

## 수동 진단

```bash
mkdir -p /tmp/resource-monitor-output
./build/release/resource-monitor --config ./config/settings.json --output /tmp/resource-monitor-output --interval-seconds 1 --once
```

`--once`도 CPU 차분을 위해 지정한 관측 구간을 기다린 후 한 번 수집·판정하고 전체 결과를 하나의 JSON 파일로 저장합니다. `--interval-seconds`는 1~86400 범위이며 서비스 기본값은 60입니다. `--help`와 `--version`도 제공합니다. 수동 진단은 service 계정과 동일한 권한 조건에서도 확인하십시오.

일반 JSON의 `retention`은 `log_counts` 하위 시간별 파일과 체크포인트를 삭제하지 않습니다. 시간별 집계의 7일 보존은 별도 정책입니다.

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
cp artifacts/resource-monitor-0.9.0.tar.gz "$HOME/rpmbuild/SOURCES/"
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

실제 운영 승인에는 해당 서버의 OL 버전, 커널, 마운트, SELinux, systemd 설치·기동·자동 시작·결과 생성을 별도로 확인해야 합니다. CTest는 역할 전환, 설정 재적용, 리소스·프로세스·처리 파일·중계 로그와 DB 비활성화, 5분 구간 합산, 시간 경계, 재시작 복구, 저장 실패 재시도, 파일 보존을 검증합니다.

`tests/log_volume_database_integration_test.py`는 별도 MariaDB에 시험 데이터를 준비하는 선택적 통합 테스트이며 일반 CTest에는 포함하지 않습니다. 실제 업무 DB에서 실행하면 안 됩니다. 명시적으로 허용한 `window-db` 호스트, 전용 사설망, 서버 이름, 검증 토큰, 관리자 설정과 조회 전용 계정이 모두 일치해야만 시험 테이블을 재생성합니다. 배포 데몬의 DB 동작은 이 시험 데이터 준비 코드와 별개로 조회 전용입니다.

<a id="upgrade-from-1-8"></a>

## 1.8.0 또는 1.12.0 제거 후 0.9.0 재설치 상세 가이드

이 절차는 기존 1.8.0 또는 1.12.0 RPM으로 설치한 Oracle Linux 8/9 x86_64 장비를 0.9.0으로 이전하는 가이드입니다. 기존 패키지를 제거한 다음 새 RPM을 설치하고, 장비의 기존 설정과 DB 계정을 보존하면서 새 설정 형식으로 변환합니다. 작업 중에는 모니터링이 중단됩니다. CloudESM 서비스와 MariaDB 서버는 중지하지 않습니다.

제거·설치 대상은 `resource-monitor` 패키지뿐입니다. DB 사용자·비밀번호, DB 테이블, `/CloudESM` 처리 파일을 삭제하지 않습니다. 기존 ZIP의 역할별 예제를 다시 복사하지 않고 **해당 장비에서 사용하던 설정**을 기준으로 이전합니다. 이 절차는 운영자가 수행하는 안내이며, 문서 추가 작업에서 운영 장비를 다시 설치하거나 재부팅한 것은 아닙니다.

### 1. 준비 파일과 실행 환경

아래 변수는 원래 요청한 1.8.0 배포 ZIP 기준입니다. 1.12.0에서 이전하는 경우 `rm_old_zip`, `rm_old_dir`, `rm_old_rpm`을 해당 1.12.0 설치 키트와 RPM으로 바꾸고, 복구 단계도 백업해 둔 동일 구버전으로 복원합니다. 제거·백업·새 설정 변환 순서는 같습니다. 1.12.0의 이전 역할 문자열도 반드시 새 값으로 변환해야 합니다.

서버의 `/root`에 아래 두 ZIP을 준비합니다. 원래 1.8.0 ZIP은 문제 발생 시 복구할 RPM을 확보하기 위한 것입니다. 가이드가 추가된 ZIP을 사용하거나 다른 경로에 업로드했다면 아래 변수의 파일명을 실제 이름으로 바꿉니다.

- 기존: `resource-monitor-1.8.0-install-kit-20261023.zip`
- 최신: `resource-monitor-0.9.0-install-kit-20261029.zip`

이하 `bash` 코드는 **DB의 `MariaDB>` 프롬프트가 아닌 서버의 root Bash 셸**에서 실행합니다. 같은 셸에서 순서대로 진행합니다. 중요 명령의 `|| exit 1`은 실패 시 현재 셸을 종료해 후속 제거·시작을 막습니다. 종료됐다면 오류 원인을 해결한 뒤 재접속하고 기록한 작업·백업 경로로 변수를 복구하십시오. 단계별 확인이 끝나기 전에는 다음 단계로 넘어가지 않습니다. 절차의 JSON 편집·검사에만 Python 3가 필요하며 데몬 실행 자체에는 필요하지 않습니다. 폐쇄망은 `unzip`, Python 3와 해당 OS용 RPM 의존성을 사내 저장소 또는 별도 RPM으로 먼저 준비합니다.

```bash
test "$(id -u)" -eq 0 || exit 1
id -u
cat /etc/oracle-release
uname -m
rpm -q resource-monitor
/usr/sbin/resource-monitor --version
systemctl cat resource-monitor
if [ -e /etc/resource-monitor/.settings.json.migrate ] || [ -L /etc/resource-monitor/.settings.json.migrate ] || [ -e /etc/resource-monitor/settings.json.new ] || [ -L /etc/resource-monitor/settings.json.new ]; then
    printf "이전 설정 이전 파일 .settings.json.migrate 또는 settings.json.new를 보존하고 확인한 후 진행하십시오\n" >&2
    exit 1
fi
runuser -u resource-monitor -- /usr/sbin/resource-monitor --check-config || exit 1
dnf install -y unzip python3 || exit 1
```

UID는 `0`, 아키텍처는 `x86_64`, 기존 버전과 복구 RPM의 버전이 일치하는지 확인합니다. `rpm -q`가 실패하거나 실행 경로가 `/usr/sbin/resource-monitor`가 아닌 소스 설치 장비에는 이 제거 절차를 그대로 사용하지 않습니다. `systemctl cat`에서 별도 `ExecStart`, `--config`, `--output`, `--interval-seconds` 또는 drop-in이 있는지도 확인합니다. 아래는 기본 설정 경로 `/etc/resource-monitor/settings.json` 기준이며, 사용자 정의 실행 옵션이 있으면 실제 경로와 우선 적용값을 확인해 맞춰야 합니다.

```bash
rm_old_zip=/root/resource-monitor-1.8.0-install-kit-20261023.zip
rm_new_zip=/root/resource-monitor-0.9.0-install-kit-20261029.zip
test -f "$rm_old_zip" && test -f "$rm_new_zip" || exit 1
rm_work=$(mktemp -d /root/resource-monitor-upgrade-20261029.XXXXXX) || exit 1
unzip -q "$rm_old_zip" -d "$rm_work" || exit 1
unzip -q "$rm_new_zip" -d "$rm_work" || exit 1
rm_old_dir="$rm_work/resource-monitor-1.8.0"
rm_new_dir="$rm_work/resource-monitor-0.9.0-install-kit-20261029"
(cd "$rm_old_dir" && sha256sum -c SHA256SUMS.txt) || exit 1
(cd "$rm_new_dir" && sha256sum -c SHA256SUMS.txt) || exit 1
. /etc/os-release
case "$ID:${VERSION_ID%%.*}:$(uname -m)" in
    ol:8:x86_64) rm_el=8 ;;
    ol:9:x86_64) rm_el=9 ;;
    *) printf '지원 대상 Oracle Linux 8/9 x86_64가 아닙니다.\n' >&2; exit 1 ;;
esac
rm_old_rpm="$rm_old_dir/packages/resource-monitor-1.8.0-1.el${rm_el}.x86_64.rpm"
rm_new_rpm="$rm_new_dir/packages/resource-monitor-0.9.0-1.el${rm_el}.x86_64.rpm"
rpm -qp "$rm_old_rpm" "$rm_new_rpm" || exit 1
rpm -K "$rm_old_rpm" "$rm_new_rpm" || exit 1
rpm -Uvh --oldpackage --test "$rm_new_rpm" || exit 1
```

모든 파일의 SHA-256 및 RPM digest 검사와 설치 사전검사가 성공해야 합니다. 0.9.0은 기존 1.x보다 버전 번호가 낮아 사전검사에 `--oldpackage`를 사용하며, `--test`가 있으므로 이 명령은 패키지를 변경하지 않습니다. 실제 교체는 아래 제거 후 설치 단계에서 수행합니다. `rpm -K`의 digest 검증은 배포자 서명 검증과 구분됩니다. 의존성 부족이 나오면 제거 전에 해결합니다. EL8 장비에는 EL8 RPM, EL9 장비에는 EL9 RPM을 사용하고 `--nodeps`, `--force`는 사용하지 않습니다. 셸을 다시 열면 `rm_work`, `rm_old_rpm`, `rm_new_rpm` 등의 변수를 같은 경로로 다시 지정해야 합니다.

### 2. 중지 전 상태와 실제 경로 기록

백업은 결과 저장 경로 밖의 `/var/backups/resource-monitor`에 root 전용으로 생성합니다. 다음 코드는 비밀번호를 출력하지 않고 설정의 파일 경로만 읽습니다.

```bash
umask 077
rm_backup="/var/backups/resource-monitor/$(date +%Y%m%dT%H%M%S)-1.8.0-to-0.9.0"
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
rm_analyzer=$(python3 -c 'import json; s=json.load(open("/etc/resource-monitor/settings.json")); print("true" if s.get("role") in ("anal","analyzer") or ("role" not in s and s.get("is_analyzer",False)) else "false")') || exit 1
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

### 4. 기존 패키지 제거

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

`resource-monitor 0.9.0`과 설정 원본 일치를 확인합니다. 이 시점에는 **아직 서비스를 시작하지 않습니다.** 복원 대상은 설정·계정·CA·기존 사용자 정의 단위 파일이며 새 RPM의 `/usr/sbin/resource-monitor`와 `/usr/lib/systemd/system/resource-monitor.service`를 구버전으로 덮어쓰지 않습니다. 사용자 정의 단위가 있다면 `systemctl cat resource-monitor`로 새 바이너리를 실행하는지 다시 확인합니다. 기존 결과 폴더가 보존되어 있다면 JSON 백업을 그 위에 풀 필요가 없습니다.

### 6. 0.9.0 설정 이전과 최초 자동 삭제 방지

**1.8.0은 자동 삭제가 없었지만 0.9.0은 `retention` 생략 시 기본 1시간 보존입니다.** 기존 파일을 유지하며 전환할 수 있도록 아래에서는 첫 실행의 자동 삭제를 명시적으로 끕니다. 설치 확인 후 운영 보존 정책을 선택합니다. 역할은 새 enum 값으로 변환하고 제거된 분석 설정은 삭제합니다. 수집 주기, 저장 경로, DB 연결·계정 경로, 리소스·처리 파일 임계값과 현장 DAPP 목록은 유지합니다.

```bash
install -m 0640 -o root -g resource-monitor /etc/resource-monitor/settings.json /etc/resource-monitor/settings.json.new || exit 1
python3 - /etc/resource-monitor/settings.json.new <<'PY' || exit 1
import json
import sys
path = sys.argv[1]
with open(path, encoding="utf-8") as stream:
    settings = json.load(stream)
settings["retention"] = {"enabled": False, "max_age_seconds": 3600}
mapping = {"collector": "col", "analyzer": "anal", "relay": "prx", "col": "col", "anal": "anal", "prx": "prx"}
legacy = settings.pop("is_analyzer", None)
if legacy is not None and type(legacy) is not bool:
    raise ValueError("Invalid legacy is_analyzer")
previous = settings.get("role", "analyzer" if legacy else "collector")
settings["role"] = mapping[previous]
for removed in ("analyzer", "log_silence", "log_volume"):
    settings.pop(removed, None)
settings.get("mysql", {}).pop("enabled", None)
profiles = settings.get("process_monitor", {})
for previous, current in (("collector", "col"), ("analyzer", "anal"), ("relay", "prx")):
    if previous in profiles:
        if current in profiles:
            raise ValueError("Conflicting process profile: " + current)
        profiles[current] = profiles.pop(previous)
if "anal" in profiles:
    profiles["anal"] = list(dict.fromkeys("AnalyzeEngine" if name == "SoarAnalyzerEngine" else name for name in profiles["anal"]))
with open(path, "w", encoding="utf-8") as stream:
    json.dump(settings, stream, ensure_ascii=False, indent=2)
    stream.write("\n")
PY
diff -u "$rm_backup/settings.before.json" /etc/resource-monitor/settings.json.new
runuser -u resource-monitor -- /usr/sbin/resource-monitor --config /etc/resource-monitor/settings.json.new --check-config && mv -f -- /etc/resource-monitor/settings.json.new /etc/resource-monitor/settings.json || exit 1
```

`diff`는 변경이 있으면 종료 코드 1을 반환합니다. 역할·프로세스 프로필 변환, 제거된 분석 설정 삭제, 일반 자동 삭제 비활성화, `SoarAnalyzerEngine` → `AnalyzeEngine` 변경 외의 운영값이 유지됐는지 확인합니다. 기존 `Collector`/`CloudESMCollector` 등 다른 DAPP 이름은 그대로 유지합니다. 실제 장비에 맞는 목록을 확인하되 전체 샘플 프로필로 덮어쓰지 않습니다.

`Configuration is valid`가 출력되고 설정 교체까지 성공해야 합니다. 실패하면 `.new` 파일과 자격증명 접근 권한을 수정한 뒤 다시 검증합니다. `--check-config`는 설정 문법·값과 분석 장비의 계정 파일을 검사하며 실제 DB TCP 접속, TLS 연결 성공, 결과 저장 권한까지 보증하지 않습니다. `role="col"`인 수집 장비는 계정 파일 검사와 DB 연결을 수행하지 않습니다.

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

### 7. 분석 장비의 조회 권한 확인

`role="anal"`에서만 DB 접속이 필요합니다. 기존 조회 계정과 비밀번호는 유지하고 `CloudESM.SUM_COLLECT_INFO` SELECT 권한과 실제 TCP 접속을 확인합니다. DBA가 확인할 SQL은 다음과 같습니다. 테이블·트리거·인덱스를 추가하지 않습니다.

```sql
SHOW GRANTS FOR 'resource_monitor'@'127.0.0.1';
SHOW INDEX FROM CloudESM.SUM_COLLECT_INFO;
```

기존 `recv_time` 선두 인덱스가 범위 조건을 지원하는지 실제 실행 계획을 확인합니다. 새 버전은 일별 비교·무수신 조회와 일별 테이블 접근을 실행하지 않습니다. 이전에 부여한 미사용 권한을 자동으로 회수하지 않습니다.

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

시작 직전 Python 검사는 실제 적용 파일의 자동 삭제가 꺼져 있는지 다시 확인합니다. 검사가 실패하면 보존 설정을 먼저 수정하고 시작 명령을 실행하지 않습니다. `enabled`, `active`, `resource-monitor 0.9.0`을 확인합니다. 첫 정기 파일은 시작 후 설정된 수집 주기만큼 지난 뒤 생성됩니다. 기본이면 약 60초이며 수집 작업 시간이 추가될 수 있습니다. 분석 역할은 DB 집계 결과나 오류가 먼저 준비되면 이보다 일찍 추가 JSON이 생성될 수 있습니다. 최소 두 수집 주기에 걸쳐 파일이 계속 생성되는지 확인합니다. 아래 명령은 **이번 시작 이후** 생성·수정된 JSON만 선택하여 이전 파일을 성공 증거로 잘못 사용하지 않도록 합니다.

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
| 역할·프로세스 | 분석은 `process_profile=anal`, 수집은 `col`, 중계는 `prx`; DAPP 이름과 true/false 확인 |
| 분석 DB | `log_volume`의 구간 처리 상태와 오류 확인; 첫 시간 종료 후 `log_counts/log_cnt_<시각>.json` 확인 |
| 수집 DB | `log_volume.status=disabled` |
| JSON 계약 | 임계값 숫자 필드가 없어도 정상; 실제 측정값과 판정 결과, `thresholds_applied_at`은 유지 |
| 보존 | 최초 전환 단계에서는 `retention.enabled=false`이므로 과거 파일 자동 삭제 없음 |

임계치 초과, 중계 ERROR 또는 DAPP 미실행에 따른 `warning`은 데몬 설치 실패와 구분합니다. DB 접속 실패는 `error`로 기록되지만 리소스 JSON은 계속 생성되어야 합니다. 최초 비동기 조회의 `pending`이 계속되거나 `error`가 남으면 실제 DB 연결·권한·쿼리 실행 시간을 확인합니다. 수집 서버에서 처리 파일 수가 임계치 미만이면 `processing_files` 항목이 없는 것이 정상입니다.

새 버전은 `collect_count`·`log_silence`와 일반 결과의 장비별 `log_volume.equipment`를 제거합니다. `schema_version`은 1을 유지하므로 결과 소비자는 새 역할과 필드 변경을 함께 반영해야 합니다. 시간별 건수는 별도 `log_counts/log_cnt_<시각>.json`에서 읽습니다. 기존 JSON은 다시 작성하지 않습니다.

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

### 11. 실패 시 기존 버전으로 복구

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
cp -a -- /etc/resource-monitor/settings.json "$rm_backup/settings.failed-0.9.0.json" || exit 1
journalctl -u resource-monitor -n 300 --no-pager > "$rm_backup/journal.failed-0.9.0.txt"
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

**구버전은 새 역할·출력 계약을 지원하지 않으므로 0.9.0 설정 파일을 그대로 사용하지 않습니다.** 반드시 제거 전 원본 설정·secrets를 복원하고 `resource-monitor 1.8.0`, 설정 검사 성공 및 새 JSON 생성을 확인합니다. 위 명령은 원래 상시 실행하던 장비 기준으로 자동 시작을 다시 활성화합니다. 원래 비활성 장비였다면 기록한 `enabled-before.txt`, `active-before.txt`에 맞게 상태를 복원합니다.

기존 출력 폴더가 그대로 있다면 JSON 백업을 다시 풀지 않습니다. 보존 설정으로 삭제된 과거 결과가 필요하면 아래처럼 **별도 폴더에** 복구하고 확인합니다. 실행 중 출력 폴더의 잠금 파일을 덮어쓰지 않습니다.

```bash
rm_restore_dir=$(mktemp -d "$rm_backup/recovered-output.XXXXXX") || exit 1
tar --acls --xattrs --selinux -xzpf "$rm_backup/monitor-output.tar.gz" -C "$rm_restore_dir" || exit 1
printf '과거 결과 복구 위치: %s\n' "$rm_restore_dir"
```

이 절차는 DB 인덱스나 테이블을 변경하지 않습니다. DB 계정과 원본 수집 데이터는 패키지 교체·복구 대상이 아닙니다.
