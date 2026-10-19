<!-- README.md -->
# Resource Monitor

Oracle Linux 8/9용 C++23 시스템 모니터링 데몬입니다. 기본 60초마다 CPU, 메모리, 마운트된 로컬 디스크 파일시스템을 수집하고 매 수집 주기의 전체 결과를 OS 시간대와 UTC 오프셋을 포함한 파일명으로 저장합니다. 예를 들어 OS가 Asia/Seoul이면 `/Monitor/20261019T123400+0900.json`에 저장합니다. `/etc/resource-monitor/settings.json`에서 수집 주기, 저장 경로, 임계치와 선택적 MySQL 연결을 관리합니다. DB 사용자명과 비밀번호는 `/etc/resource-monitor/secrets/mysql.json`으로 분리합니다. 임계치는 `status` 판정에만 적용합니다.

## 구성

| 위치 | 책임 |
| --- | --- |
| `src/domain` | 측정 모델, 계산식, 임계치 규칙, 상태 집계 |
| `src/application` | 수집·설정 전체 적용·실패 시 기존 설정 유지·저장 유스케이스와 포트 |
| `src/infrastructure/linux` | procfs, mountinfo, 파일시스템 용량 수집 |
| `src/infrastructure/config` | 사용자 설정·MySQL 계정 JSON 파싱·검증과 설정 포트 구현 |
| `src/infrastructure/database` | MySQL 연결 준비·교체와 연결 확인 |
| `src/infrastructure/persistence` | 저장 경로 검증·잠금·전환, JSON 직렬화와 원자적 파일 저장 |
| `src/runtime` | 단조 시계 수집·설정 확인 타이머, 종료 신호 |
| `src/main.cpp` | 구성 객체 생성과 의존성 주입 |
| `tests` | 계산·설정·저장·실행 통합·의존성 검사 |
| `packaging` | systemd 서비스, RPM 명세, Docker 빌드 환경 |

Domain은 Linux와 JSON을 참조하지 않습니다. Application은 자신의 포트와 Domain에만 의존합니다. 운영체제 호출과 JSON 라이브러리는 외부 계층에 있습니다. 테스트에서 의존성 방향과 작성한 소스의 파일 상단 경로 주석 규칙을 검사합니다.

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
  "collection": {
    "interval_seconds": 60,
    "output_directory": "/Monitor"
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
| `is_analyzer` | JSON 불리언 `true` 또는 `false`, 생략 시 기본 `false` | 다음 설정 확인 후 DB 연결·쿼리 허용 여부 변경 |
| `collection.interval_seconds` | 1~86400의 정수, 기본 60초 | 다음 설정 확인 후 새 주기 시작 |
| `collection.output_directory` | 제어문자가 없는 절대 경로, 기본 `/Monitor` | 다음 설정 확인 후 저장부터 |
| `thresholds.cpu_usage_percent` | 0~100의 숫자 | 다음 설정 확인 후 수집부터 |
| `thresholds.memory_usage_percent` | 0~100의 숫자 | 다음 설정 확인 후 수집부터 |
| `thresholds.storage_usage_percent` | 0~100의 숫자 | 다음 설정 확인 후 수집부터 |

세 임계치는 필수이며 유한한 숫자여야 합니다. 0은 항상 경고를 발생시킵니다. `collection`이나 그 하위 설정을 생략하면 해당 기본값을 사용하므로 이전 임계치 전용 JSON도 읽을 수 있습니다. 잘못된 자료형, null, 필수 키 누락, 중복 키, 알 수 없는 키, 지원하지 않는 스키마 버전은 거부합니다. 설정은 최대 64KiB의 일반 파일이어야 하며 심볼릭 링크와 FIFO는 허용하지 않습니다. 새로운 동작 설정은 이름·자료형·검증 규칙을 정의해 추가합니다.

데몬은 수집 주기와 별도로 1초마다 설정 파일 전체를 확인하므로 재시작 없이 모든 항목을 자동 반영합니다. `is_analyzer`가 `true`인 경우에만 계정 파일을 확인하고 DB 연결을 준비합니다. 파일을 직접 수정하는 방식과 새 파일로 원자 교체하는 방식 모두 지원합니다. 진행 중인 동기 수집·파일 작업이 오래 걸리면 확인도 그 작업이 끝난 뒤 수행됩니다. 명시한 `--interval-seconds`와 `--output` 옵션은 실행 중에도 각각 JSON의 값을 덮어씁니다. 기본 systemd 서비스는 이 옵션들을 지정하지 않아 파일 설정을 사용합니다.

수집 주기가 바뀌면 적용한 시점부터 새 주기만큼 기다린 뒤 다음 JSON을 저장합니다. 예를 들어 60초에서 5초로 변경하면 다음 설정 확인 후 약 5초 뒤 수집합니다. CPU 기준 샘플을 버리지 않으므로 전환 직후 `cpu.window_seconds`는 직전 샘플부터의 실제 관측 시간이며 새 주기와 다를 수 있습니다. 저장 경로나 임계치만 바뀌거나 문서 내용이 동일하면 수집 타이머를 다시 시작하지 않습니다.

저장 경로를 바꾸려면 먼저 로컬 디렉터리를 만들고 `resource-monitor` 계정에 쓰기·탐색 권한을 부여해야 합니다. 데몬은 임의의 새 경로를 자동 생성하지 않습니다. 별도 마운트에 저장한다면 `systemctl edit resource-monitor`의 `[Unit]`에 `RequiresMountsFor=/새/저장/경로`도 추가하고 SELinux 접근을 확인하십시오. 기본 단위 파일의 마운트 의존성은 `/Monitor`입니다.

사용률이 임계치 이상이면 `warning`, 미만이면 `ok`, 측정 불가면 `unknown`입니다. 표시를 위해 값을 먼저 반올림하지 않습니다. 스토리지는 파일시스템 각각에 같은 임계치를 적용합니다. 임계치 아래로 내려오면 다음 측정부터 즉시 `ok`로 돌아옵니다.

설정 파일 검증, 새 저장 경로 준비와 필요한 MySQL 연결 준비를 모두 성공한 경우에만 수집 주기·경로·임계치·DB 설정을 한 번에 적용합니다. 새 경로는 디렉터리 잠금을 확보하고 실제 임시파일 쓰기·동기화·정리를 확인한 뒤 전환합니다. 기존 경로의 잠금은 전환 완료 시 해제합니다. 같은 디렉터리를 가리키는 경로 별칭은 기존 잠금을 재사용합니다.

문서 오류, 없는 경로, 권한 부족, 다른 데몬의 잠금 등으로 변경을 적용하지 못하면 기존 설정 전체로 수집을 계속하고 JSON에 `config_status=using_last_valid`와 오류를 기록합니다. 실패 원인은 journal에도 남깁니다. 매초 다시 시도하므로 디렉터리를 준비하거나 잠금을 해제하면 파일을 다시 수정하지 않아도 자동 반영합니다. DB 연결 실패의 재시도 간격은 5초입니다. 시작부터 설정 문서가 잘못된 경우 종료 코드 78로 실패하며 systemd는 이 코드로 반복 재시작하지 않습니다. `--check-config`는 문서 형식과 `is_analyzer`가 `true`일 때 계정 파일을 검증합니다. DB 네트워크 접속과 실제 저장 경로 준비는 실행 시 검증합니다.

사용자가 수정한 파일을 데몬이 덮어쓰지 않습니다. 편집 중간의 일시적 파싱 실패를 줄이려면 같은 디렉터리에 새 파일을 작성·검증한 뒤 원자적으로 교체하십시오. 별도 검증 명령은 다음과 같습니다.

```bash
resource-monitor --config /etc/resource-monitor/settings.json --check-config
```

## MySQL 연결 설정

MySQL 연결과 쿼리는 최상위 `is_analyzer`가 `true`일 때만 실행합니다. `is_analyzer`는 생략하면 `false`이며 문자열 `"true"`, 숫자 `1`, `null`은 허용하지 않습니다. JSON에는 소문자 `true` 또는 `false`를 입력하십시오. `is_analyzer`가 `true`이면 `mysql` 객체와 `mysql.database`를 반드시 지정해야 합니다. `settings.json`의 `mysql`에는 접속 대상과 동작 설정만 저장하고 `username` 또는 `password`를 직접 넣으면 설정을 거부합니다.

1.3.3부터 `mysql.enabled`를 제거하고 `is_analyzer` 하나로 DB 사용 여부를 제어합니다. 업그레이드 전에 기존 `settings.json`의 `mysql.enabled`를 삭제하고 원하는 동작에 맞게 최상위 `is_analyzer`를 지정하십시오. 제거된 키가 남아 있으면 알 수 없는 키로 설정을 거부하며, RPM 업그레이드는 기존 사용자 설정을 자동으로 수정하지 않습니다. `is_analyzer`가 없거나 `false`이면 DB에 연결하거나 쿼리를 실행하지 않고 계정 파일도 읽지 않습니다. CPU·메모리·스토리지 수집과 JSON 저장은 계속합니다.

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

DB 사용이 허용된 동안 설정과 계정 파일을 1초마다 다시 읽으므로 비밀번호만 바꾸어도 재연결합니다. 새 연결과 `SELECT 1` 확인은 별도 작업 스레드에서 수행하여 기존 리소스 수집을 계속합니다. 준비된 연결이 성공하고 설정 전체를 적용할 수 있을 때 기존 연결을 교체합니다. 그동안 주소·계정 오류, 접속 거부, TLS 오류가 발생하면 기존 연결과 설정 전체를 유지하고 5초 간격으로 다시 시도합니다. 대기 중 파일을 다시 수정하면 최신 설정에 맞는 연결만 채택합니다.

`is_analyzer`를 `false`로 바꾸거나 삭제하면 다음 설정 확인에서 기존 연결을 닫고 새 DB 연결·쿼리·계정 파일 읽기를 중단합니다. 데몬을 재시작할 필요가 없습니다. 이미 작업 스레드에 전달된 연결이나 쿼리는 완료될 수 있지만 비활성화 이전 작업의 결과는 채택하지 않습니다. 다시 `is_analyzer`를 `true`로 설정하면 최신 설정과 계정 파일로 연결을 준비합니다.

처음 실행할 때 DB에 연결할 수 없거나 계정 파일이 잘못되어도 리소스 수집은 시작 설정의 수집 주기·저장 경로·임계치로 계속합니다. 연결이 준비될 때까지 DB는 비활성 상태이고 `config_status=using_last_valid`와 계정 값을 제외한 오류를 기록합니다. DB 접속 성공 후에는 정상 상태로 돌아옵니다. DB 오류는 CPU·메모리·스토리지 사용률에 대한 `status`와 별개입니다. 사용자명·비밀번호와 DB 서버가 반환한 원문 오류는 journal 및 결과 JSON에 기록하지 않습니다.

DB 사용이 허용된 정상 연결은 30초마다 `SELECT 1`로 확인하며 끊어진 연결은 현재 설정으로 다시 준비합니다. 저장 경로 오류 등으로 적용을 기다리는 후보 연결도 30초가 지나면 새로 연결·검증합니다. 시간 제한은 연결 라이브러리의 네트워크 작업에 적용되며 운영체제의 DNS 조회 시간을 제한하지는 않습니다. 종료 시 진행 중인 DB 작업을 기다리고, systemd의 기존 `TimeoutStopSec=15`를 초과하면 systemd가 프로세스를 종료합니다.

이 버전은 접속 설정과 계정 분리, 실제 연결·검증·교체까지 제공합니다. 연결 확인용 `SELECT 1` 외의 업무 쿼리나 조회 결과 저장은 아직 실행하지 않습니다. 쿼리와 결과 계약이 정해지면 응용 계층의 DB 포트를 통해 추가할 수 있습니다.

## JSON 계약

| 필드 | 값과 의미 |
| --- | --- |
| `schema_version` | 1 |
| `timestamp` | OS 시간대와 UTC 오프셋을 포함한 ISO 8601 수집 시각, 예: `2026-10-19T12:34:00+09:00` |
| `thresholds_applied_at` | 현재 임계치가 적용된 시각을 OS 시간대로 표시한 값, 밀리초와 UTC 오프셋 포함, 예: `2026-10-19T12:33:00.123+09:00` |
| `hostname` | 수집 호스트 |
| `status` | `ok`, `warning`, `unknown` |
| `collection_status` | `ok`, `partial`, `error` |
| `config_status` | `ok`, `using_last_valid` |
| `applied_thresholds` | 이 결과에 실제 적용한 세 임계치 |
| `missed_intervals` | 이번 결과 전에 건너뛴 실행 주기 수 |
| `cpu` | 관측 구간 초, 사용률, iowait 비율, steal 비율, 상태 |
| `memory` | 전체·사용·가용 bytes, 사용률, 상태 |
| `filesystems` | 장치, 장치 식별자, 마운트 목록, 유형, 전체·사용·미사용·가용 bytes, 사용률, 상태 |
| `errors` | component, target, code, message로 구성된 오류 목록 |

전체 `status`는 하나라도 경고면 `warning`, 경고 없이 측정 불가가 있으면 `unknown`, 모두 정상 측정되고 임계치 미만이면 `ok`입니다. `ok`, `warning`, `unknown` 모두 매 수집 주기에 JSON으로 기록합니다. 수집 실패와 설정 오류는 별도 축입니다. 예를 들어 디스크 하나가 경고이고 다른 디스크가 수집 실패이면 `status=warning`, `collection_status=partial`인 전체 결과를 저장합니다. 설정 오류만 발생하면 `collection_status`를 변경하지 않습니다. 실행 중 발생한 수집·설정 오류는 해당 주기의 JSON `errors`와 journal에 기록합니다.

측정할 수 없는 값은 0 대신 JSON null입니다. 이전 결과를 현재 측정값처럼 재사용하지 않습니다. 빈 파일시스템 목록은 모든 디스크 정상으로 취급하지 않습니다.

파일명은 `YYYYMMDDTHHMMSS+HHMM.json` 또는 음수 오프셋의 같은 형식을 사용합니다. UTC로 설정된 OS에서도 JSON에는 `+00:00`, 파일명에는 `+0000`을 명시하며 `Z`를 붙이지 않습니다. 시간대가 일광 절약 시간을 사용하면 해당 시각의 오프셋을 기록하므로 시계가 되돌아가 같은 지역 시각이 반복되어도 파일명을 구분할 수 있습니다. 이전 버전에서 생성한 `Z` 형식의 파일도 그대로 보존합니다.

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

시작 시 CPU 기준값을 확보하고 기본 60초 후 첫 수집과 임계치 판정을 수행하여 전체 결과를 저장합니다. 이후 매 수집 주기에 수집, CPU 기준값 갱신, 임계치 판정과 JSON 저장을 수행합니다. 경고에서 정상으로 복구된 주기와 측정할 수 없는 항목이 있는 주기도 같은 JSON 형식으로 기록합니다. Linux CLOCK_MONOTONIC 타이머를 사용하므로 OS 시간대나 UTC/NTP 시간 변경이 주기를 바꾸지 않습니다. 작업 중 지나간 주기는 건너뛰고 다음 주기를 기다립니다. 실제 CPU 관측 구간은 cpu.window_seconds에 기록됩니다.

저장할 때마다 `tzset()`으로 시스템 시간대 정보를 갱신하고 JSON 시각과 파일명에 같은 시간대를 적용합니다. 기본 systemd 서비스처럼 `TZ` 환경 변수를 별도로 지정하지 않은 경우 `/etc/localtime`을 따르며, OS 시간대 변경은 데몬 재시작 없이 다음 저장부터 반영됩니다. 별도로 `TZ`를 지정해 실행하면 해당 프로세스의 시간대 설정이 우선합니다. 시간대 변경은 시각의 표시만 바꾸며 실제 수집 시점과 임계치 적용 시점은 유지합니다.

SIGTERM/SIGINT는 signalfd로 처리하며 대기 중 즉시 종료할 수 있습니다. 진행 중인 동기 파일시스템 작업은 끝난 뒤 종료 신호를 처리합니다. 저장 장치 자체가 커널에서 장시간 멈춘 경우에는 즉시 종료를 보장하지 않습니다. systemd의 종료 제한 시간은 15초입니다.

매 수집 주기에 임시 파일을 출력 디렉터리에 O_EXCL로 생성합니다. 모든 내용 기록과 파일 fsync 뒤 덮어쓰기 없는 원자적 이름 확정을 수행하고 디렉터리를 fsync합니다. 파일명이 이미 있으면 `_1`, `_2` 순번을 붙여 기존 파일을 보존합니다. 저장 실패는 journal에 기록하며, 다음 주기에 새로 수집한 결과의 저장을 다시 시도합니다. 실패한 과거 결과는 재전송하지 않습니다. 실패한 주기를 메모리에 무한 적재하거나 과거 파일을 삭제하지 않습니다. crash로 남은 `.tmp`는 결과 파일이 아니며, 자동으로 기존 파일을 삭제하지 않습니다.

기본 결과 파일 권한은 0640이며 systemd의 umask 0027이 적용됩니다. `.resource-monitor.lock`은 동일 출력 디렉터리에 대한 중복 실행을 막는 잠금 파일입니다. 저장기는 잠금을 확보한 디렉터리 FD에 결과를 기록하며, 경로가 외부에서 교체되어도 다른 디렉터리를 임의로 따라가지 않습니다. 실행 중 잠금 파일을 삭제하거나 교체하지 마십시오. 이전·준비 실패 경로의 잠금 파일은 남을 수 있으며 파일 존재 자체가 잠금 보유를 뜻하지는 않습니다.

기본 60초 주기로 정상 실행되고 저장에 성공하면 한 호스트에서 하루 약 1,440개 파일이 생성됩니다. 자동 삭제 기능은 없으므로 보존 기간에 맞는 별도 보관·정리 정책이 필요합니다.

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
cp artifacts/resource-monitor-1.3.3.tar.gz "$HOME/rpmbuild/SOURCES/"
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

실제 운영 승인에는 대상 서버의 OL 마이너 버전, RHCK/UEK, 마운트 구성, SELinux Enforcing 환경에서 설치·기동·60초 수집·상태별 JSON 저장·재시작을 확인하는 절차가 필요합니다. 테스트는 CPU 경계, 임계치 재로드, 매 주기 저장, 예약 공간, 부분 실패, 원자 저장 실패, 마운트 파싱, FD 정리, 종료 신호를 검증합니다.
