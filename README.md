<!-- README.md -->
# Resource Monitor

Oracle Linux 8/9용 C++23 시스템 모니터링 데몬입니다. 기본 60초마다 CPU, 메모리, 마운트된 로컬 디스크 파일시스템을 수집하고 매 수집 주기의 전체 결과를 `/Monitor/YYYYMMDDTHHMMSSZ.json`에 저장합니다. `/etc/resource-monitor/settings.json`에서 수집 주기, 저장 경로와 임계치를 관리합니다. 임계치는 `status` 판정에만 적용합니다.

## 구성

| 위치 | 책임 |
| --- | --- |
| `src/domain` | 측정 모델, 계산식, 임계치 규칙, 상태 집계 |
| `src/application` | 수집·설정 전체 적용·실패 시 기존 설정 유지·저장 유스케이스와 포트 |
| `src/infrastructure/linux` | procfs, mountinfo, 파일시스템 용량 수집 |
| `src/infrastructure/config` | 사용자 설정 JSON 파싱·검증과 설정 포트 구현 |
| `src/infrastructure/persistence` | 저장 경로 검증·잠금·전환, JSON 직렬화와 원자적 파일 저장 |
| `src/runtime` | 단조 시계 수집·설정 확인 타이머, 종료 신호 |
| `src/main.cpp` | 구성 객체 생성과 의존성 주입 |
| `tests` | 계산·설정·저장·실행 통합·의존성 검사 |
| `packaging` | systemd 서비스, RPM 명세, Docker 빌드 환경 |

Domain은 Linux와 JSON을 참조하지 않습니다. Application은 자신의 포트와 Domain에만 의존합니다. 운영체제 호출과 JSON 라이브러리는 외부 계층에 있습니다. 테스트에서 의존성 방향과 작성한 소스의 파일 상단 경로 주석 규칙을 검사합니다.

## 빌드

필수 도구는 C++23 및 `std::expected`를 지원하는 GCC Toolset 15, CMake 3.20 이상, Make입니다. 테스트에는 Python 3 표준 라이브러리를 사용합니다. 런타임에는 Python이 필요하지 않습니다. JSON 라이브러리 nlohmann/json 3.12.0은 CMake가 공식 배포본을 SHA-256 검증 후 가져옵니다.

Oracle Linux의 활성 AppStream 저장소에 해당 패키지가 있어야 합니다. OL8/9 각각의 대상 배포판에서 별도로 빌드하고 설치하십시오.

```bash
sudo dnf install -y gcc-toolset-15-gcc-c++ cmake make python3
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
sudo /usr/sbin/resource-monitor --check-config
sudo systemctl daemon-reload
sudo systemctl enable --now resource-monitor
systemctl status resource-monitor
journalctl -u resource-monitor -f
```

기존 `settings.json`은 CMake 재설치와 RPM 업그레이드 시 보존합니다. 새 파일이 없고 이전 `thresholds.json`만 있으면 기존 내용을 `settings.json`으로 복사합니다. 기존 파일의 값이 잘못되어 있어도 기본값으로 바꾸지 않습니다. 기본 예제는 `/usr/share/resource-monitor/settings.json`에 별도로 설치됩니다. `/Monitor`는 로컬 파일시스템에 두고 서비스 계정이 쓰기 가능해야 합니다. 설치 명령은 관리자 권한이 필요하지만 데몬은 전용 계정으로 실행됩니다.

SELinux가 Enforcing인 실제 서버에서 파일 쓰기와 디렉터리 접근을 검증하십시오. 이 패키지는 SELinux를 해제하거나 별도 정책을 자동 적용하지 않습니다. 모니터링 대상 경로를 서비스 계정이 탐색할 수 없으면 해당 항목에 접근 오류가 기록됩니다. systemd 단위는 마운트 네임스페이스를 격리하는 옵션을 사용하지 않습니다.

## 사용자 설정 수정

`/etc/resource-monitor/settings.json`의 예시입니다.

```json
{
  "schema_version": 1,
  "collection": {
    "interval_seconds": 60,
    "output_directory": "/Monitor"
  },
  "thresholds": {
    "cpu_usage_percent": 80.0,
    "memory_usage_percent": 85.0,
    "storage_usage_percent": 90.0
  }
}
```

| 설정 | 허용 값과 기본값 | 변경 적용 시점 |
| --- | --- | --- |
| `collection.interval_seconds` | 1~86400의 정수, 기본 60초 | 다음 설정 확인 후 새 주기 시작 |
| `collection.output_directory` | 제어문자가 없는 절대 경로, 기본 `/Monitor` | 다음 설정 확인 후 저장부터 |
| `thresholds.cpu_usage_percent` | 0~100의 숫자 | 다음 설정 확인 후 수집부터 |
| `thresholds.memory_usage_percent` | 0~100의 숫자 | 다음 설정 확인 후 수집부터 |
| `thresholds.storage_usage_percent` | 0~100의 숫자 | 다음 설정 확인 후 수집부터 |

세 임계치는 필수이며 유한한 숫자여야 합니다. 0은 항상 경고를 발생시킵니다. `collection`이나 그 하위 설정을 생략하면 해당 기본값을 사용하므로 이전 임계치 전용 JSON도 읽을 수 있습니다. 잘못된 자료형, null, 필수 키 누락, 중복 키, 알 수 없는 키, 지원하지 않는 스키마 버전은 거부합니다. 설정은 최대 64KiB의 일반 파일이어야 하며 심볼릭 링크와 FIFO는 허용하지 않습니다. 새로운 동작 설정은 이름·자료형·검증 규칙을 정의해 추가합니다.

데몬은 수집 주기와 별도로 1초마다 설정 파일 전체를 확인하므로 재시작 없이 모든 항목을 자동 반영합니다. 파일을 직접 수정하는 방식과 새 파일로 원자 교체하는 방식 모두 지원합니다. 진행 중인 동기 수집·파일 작업이 오래 걸리면 확인도 그 작업이 끝난 뒤 수행됩니다. 명시한 `--interval-seconds`와 `--output` 옵션은 실행 중에도 각각 JSON의 값을 덮어씁니다. 기본 systemd 서비스는 이 옵션들을 지정하지 않아 파일 설정을 사용합니다.

수집 주기가 바뀌면 적용한 시점부터 새 주기만큼 기다린 뒤 다음 JSON을 저장합니다. 예를 들어 60초에서 5초로 변경하면 다음 설정 확인 후 약 5초 뒤 수집합니다. CPU 기준 샘플을 버리지 않으므로 전환 직후 `cpu.window_seconds`는 직전 샘플부터의 실제 관측 시간이며 새 주기와 다를 수 있습니다. 저장 경로나 임계치만 바뀌거나 문서 내용이 동일하면 수집 타이머를 다시 시작하지 않습니다.

저장 경로를 바꾸려면 먼저 로컬 디렉터리를 만들고 `resource-monitor` 계정에 쓰기·탐색 권한을 부여해야 합니다. 데몬은 임의의 새 경로를 자동 생성하지 않습니다. 별도 마운트에 저장한다면 `systemctl edit resource-monitor`의 `[Unit]`에 `RequiresMountsFor=/새/저장/경로`도 추가하고 SELinux 접근을 확인하십시오. 기본 단위 파일의 마운트 의존성은 `/Monitor`입니다.

사용률이 임계치 이상이면 `warning`, 미만이면 `ok`, 측정 불가면 `unknown`입니다. 표시를 위해 값을 먼저 반올림하지 않습니다. 스토리지는 파일시스템 각각에 같은 임계치를 적용합니다. 임계치 아래로 내려오면 다음 측정부터 즉시 `ok`로 돌아옵니다.

설정 파일 검증과 새 저장 경로 준비를 모두 성공한 경우에만 수집 주기·경로·임계치를 한 번에 적용합니다. 새 경로는 디렉터리 잠금을 확보하고 실제 임시파일 쓰기·동기화·정리를 확인한 뒤 전환합니다. 기존 경로의 잠금은 전환 완료 시 해제합니다. 같은 디렉터리를 가리키는 경로 별칭은 기존 잠금을 재사용합니다.

문서 오류, 없는 경로, 권한 부족, 다른 데몬의 잠금 등으로 변경을 적용하지 못하면 기존 설정 전체로 수집을 계속하고 JSON에 `config_status=using_last_valid`와 오류를 기록합니다. 실패 원인은 journal에도 남깁니다. 매초 다시 시도하므로 디렉터리를 준비하거나 잠금을 해제하면 파일을 다시 수정하지 않아도 자동 반영합니다. 시작부터 설정 문서가 잘못된 경우 종료 코드 78로 실패하며 systemd는 이 코드로 반복 재시작하지 않습니다. `--check-config`는 문서 형식을 검증하고 실제 저장 경로 준비는 실행 시 검증합니다.

사용자가 수정한 파일을 데몬이 덮어쓰지 않습니다. 편집 중간의 일시적 파싱 실패를 줄이려면 같은 디렉터리에 새 파일을 작성·검증한 뒤 원자적으로 교체하십시오. 별도 검증 명령은 다음과 같습니다.

```bash
resource-monitor --config /etc/resource-monitor/settings.json --check-config
```

## JSON 계약

| 필드 | 값과 의미 |
| --- | --- |
| `schema_version` | 1 |
| `timestamp` | UTC ISO 8601 수집 시각 |
| `thresholds_applied_at` | 현재 임계치가 적용된 UTC 시각 |
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

시작 시 CPU 기준값을 확보하고 기본 60초 후 첫 수집과 임계치 판정을 수행하여 전체 결과를 저장합니다. 이후 매 수집 주기에 수집, CPU 기준값 갱신, 임계치 판정과 JSON 저장을 수행합니다. 경고에서 정상으로 복구된 주기와 측정할 수 없는 항목이 있는 주기도 같은 JSON 형식으로 기록합니다. Linux CLOCK_MONOTONIC 타이머를 사용하므로 UTC/NTP 시간 변경이 주기를 바꾸지 않습니다. 작업 중 지나간 주기는 건너뛰고 다음 주기를 기다립니다. 실제 CPU 관측 구간은 cpu.window_seconds에 기록됩니다.

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
sudo dnf install -y rpm-build gcc-toolset-15-gcc-c++ cmake make python3
mkdir -p "$HOME/rpmbuild/SOURCES" "$HOME/rpmbuild/SPECS"
cp artifacts/resource-monitor-1.2.0.tar.gz "$HOME/rpmbuild/SOURCES/"
curl --fail --location https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz --output "$HOME/rpmbuild/SOURCES/json.tar.xz"
cp packaging/rpm/resource-monitor.spec "$HOME/rpmbuild/SPECS/"
rpmbuild -ba "$HOME/rpmbuild/SPECS/resource-monitor.spec"
```

RPM 설치는 전용 계정과 `/Monitor` 디렉터리를 생성합니다. 서비스 자동 시작은 설치 후 관리자가 `systemctl enable --now resource-monitor`로 선택합니다. 사용자 설정은 `%config(noreplace)`로 보존하며 `/Monitor`의 결과 파일은 패키지 소유 파일로 등록하지 않습니다. 이전 파일을 이전할 때 `thresholds.json.legacy`에 원본도 보관하며 기존 백업은 덮어쓰지 않습니다. 이후 편집 대상은 `settings.json`입니다. 패키지 기본 예제가 `.rpmnew`로 남을 수 있으므로 필요한 새 항목만 참고해 기존 설정에 추가하십시오.

Docker 개발 환경은 다음처럼 만들 수 있습니다. 컨테이너 테스트는 Oracle Linux 커널 및 실제 systemd 부팅 검증과 구분해야 합니다.

```bash
docker build -f packaging/Dockerfile.build --build-arg ORACLE_VERSION=8 -t resource-monitor-build:ol8 .
docker build -f packaging/Dockerfile.build --build-arg ORACLE_VERSION=9 -t resource-monitor-build:ol9 .
```

실제 운영 승인에는 대상 서버의 OL 마이너 버전, RHCK/UEK, 마운트 구성, SELinux Enforcing 환경에서 설치·기동·60초 수집·상태별 JSON 저장·재시작을 확인하는 절차가 필요합니다. 테스트는 CPU 경계, 임계치 재로드, 매 주기 저장, 예약 공간, 부분 실패, 원자 저장 실패, 마운트 파싱, FD 정리, 종료 신호를 검증합니다.
