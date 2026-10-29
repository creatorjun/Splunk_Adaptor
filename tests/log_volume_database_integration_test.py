# tests/log_volume_database_integration_test.py
import argparse
import contextlib
import datetime
import ipaddress
import json
import os
import pathlib
import signal
import socket
import subprocess
import tempfile
import time


def require_environment():
    required = ('DB_ALLOW_RESET', 'DB_HOST', 'DB_EXPECTED_HOSTNAME',
                'DB_EXPECTED_SUBNET', 'DB_TEST_TOKEN', 'DB_ADMIN_CONFIG',
                'DB_CREDENTIALS_FILE')
    missing = [name for name in required if not os.environ.get(name)]
    if missing:
        raise RuntimeError('Explicit isolated database configuration required: ' + ', '.join(missing))
    if os.environ['DB_ALLOW_RESET'] != '1' or os.environ['DB_HOST'] != 'window-db':
        raise RuntimeError('Database resets require DB_ALLOW_RESET=1 and DB_HOST=window-db')
    network = ipaddress.ip_network(os.environ['DB_EXPECTED_SUBNET'])
    if not network.is_private:
        raise RuntimeError('The fixture subnet must be private')
    addresses = {ipaddress.ip_address(item[4][0]) for item in socket.getaddrinfo('window-db', 3306)}
    if not addresses or any(address not in network for address in addresses):
        raise RuntimeError('window-db resolved outside the explicit isolated subnet')
    admin = pathlib.Path(os.environ['DB_ADMIN_CONFIG'])
    credentials = pathlib.Path(os.environ['DB_CREDENTIALS_FILE'])
    if not admin.is_file() or not credentials.is_file():
        raise RuntimeError('Dedicated fixture administrator and SELECT credentials are required')
    return admin, credentials


def sql(admin, statement):
    result = subprocess.run(
        ['mysql', '--defaults-file=' + str(admin), '--protocol=TCP', '--host=window-db',
         '--port=3306', '--user=fixture_admin', '--batch', '--raw', '--skip-column-names', 'CloudESM'],
        input=statement, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        universal_newlines=True, timeout=20,
    )
    if result.returncode:
        raise RuntimeError('Isolated fixture SQL failed: ' + result.stderr)
    return result.stdout.strip()


def verify_database(admin):
    hostname = sql(admin, 'SELECT @@hostname;')
    if hostname != os.environ['DB_EXPECTED_HOSTNAME']:
        raise RuntimeError('Database hostname does not match the isolated fixture')
    token = sql(admin, 'SELECT token FROM CloudESM._resource_monitor_test_guard;')
    if token != os.environ['DB_TEST_TOKEN']:
        raise RuntimeError('Dedicated database guard is missing or different; reset refused')
    return sql(admin, 'SELECT VERSION();')


def reset_tables(admin):
    verify_database(admin)
    sql(admin, '''
DROP TABLE IF EXISTS CloudESM.SUM_COLLECT_INFO;
DROP TABLE IF EXISTS CloudESM.SUM_COLLECT_INFO_1DAY;
CREATE TABLE CloudESM.SUM_COLLECT_INFO (
    agent_ip VARCHAR(39) NULL,
    connect_ip VARCHAR(39) NULL,
    log_type_nm VARCHAR(50) NULL,
    log_cate_cd VARCHAR(20) NULL,
    mac_addr VARCHAR(20) NULL,
    recv_time BIGINT NULL,
    reg_time BIGINT NULL,
    eqp_ip VARCHAR(39) NULL,
    `count` INT NULL,
    length BIGINT NULL,
    KEY recv_time (recv_time),
    KEY reg_time (reg_time)
);
CREATE TABLE CloudESM.SUM_COLLECT_INFO_1DAY (
    recv_time BIGINT NOT NULL DEFAULT 0,
    eqp_ip VARCHAR(39) NOT NULL DEFAULT '',
    log_cate_cd VARCHAR(20) NOT NULL DEFAULT '',
    `count` BIGINT NULL,
    length BIGINT NULL,
    PRIMARY KEY (recv_time, eqp_ip, log_cate_cd)
);
''')


def bucket(timestamp):
    return time.strftime('%Y%m%d%H%M', time.localtime(timestamp))


def seed_tables(admin):
    while 300 - time.time() % 300 < 15:
        time.sleep(0.2)
    end = int(time.time()) // 300 * 300
    start = end - 300
    values = [
        (start - 60, '192.0.2.10', 'outside-before', 10000),
        (start, '192.0.2.10', 'category-a', 7),
        (start + 60, '192.0.2.10', 'category-b', 11),
        (end - 60, '192.0.2.10', 'category-a', 13),
        (end, '192.0.2.10', 'outside-after', 20000),
        (start + 60, '192.0.2.20', 'category-a', 5),
        (end - 60, '192.0.2.20', 'category-b', 8),
        (start, '2001:db8::1', 'category-zero', 0),
    ]
    rows = ','.join("(%s,'%s','%s',%d)" % (bucket(stamp), address, category, count)
                    for stamp, address, category, count in values)
    sql(admin, 'INSERT INTO CloudESM.SUM_COLLECT_INFO (recv_time,eqp_ip,log_cate_cd,`count`) VALUES ' + rows + ';')
    today = datetime.date.today()
    yesterday = today - datetime.timedelta(days=1)
    older = today - datetime.timedelta(days=2)
    days = [
        (older, '192.0.2.10', 'older', 30000),
        (yesterday, '192.0.2.10', 'category-a', 100),
        (yesterday, '192.0.2.10', 'category-b', 200),
        (yesterday, '192.0.2.20', 'category-a', 40),
        (today, '192.0.2.10', 'today', 40000),
    ]
    rows = ','.join("(%s,'%s','%s',%d)" % (day.strftime('%Y%m%d'), address, category, count)
                    for day, address, category, count in days)
    sql(admin, 'INSERT INTO CloudESM.SUM_COLLECT_INFO_1DAY (recv_time,eqp_ip,log_cate_cd,`count`) VALUES ' + rows + ';')
    return start, end, yesterday, today


def write_settings(path, settings):
    replacement = path.with_suffix('.new')
    replacement.write_text(json.dumps(settings), encoding='utf-8')
    replacement.replace(path)


@contextlib.contextmanager
def daemon(binary, settings, log):
    with log.open('w', encoding='utf-8') as stream:
        process = subprocess.Popen([binary, '--config', str(settings)], stdout=stream, stderr=stream)
        try:
            yield process
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            if process.returncode != 0:
                raise AssertionError(log.read_text(encoding='utf-8'))


def wait_snapshot(output, seen, predicate, process, log, timeout=10):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(log.read_text(encoding='utf-8'))
        for path in sorted(output.glob('*.json')):
            if path.name in seen:
                continue
            payload = json.loads(path.read_text(encoding='utf-8'))
            seen.add(path.name)
            last = payload
            assert payload['config_status'] == 'ok', payload
            if predicate(payload):
                return payload
        time.sleep(0.05)
    raise AssertionError('Expected snapshot not observed: %r\n%s' % (last, log.read_text(encoding='utf-8')))


def local_stamp(epoch):
    return datetime.datetime.fromtimestamp(epoch).astimezone().isoformat(timespec='milliseconds')


def assert_volume(payload, mode, expected):
    volume = payload['log_volume']
    assert volume['mode'] == mode, volume
    assert volume['status'] == ('ok' if expected else 'no_data'), volume
    assert {item['eqp_ip']: item['count'] for item in volume['equipment']} == expected, volume
    assert volume['equipment_count'] == len(expected), volume
    assert volume['error'] is None and volume['queried_at'], volume
    assert volume['source'] == 'CloudESM.SUM_COLLECT_INFO' + ('_1DAY' if mode == 'daily' else ''), volume
    assert payload['memory']['total_bytes'] > 0 and payload['cpu']['usage_percent'] is not None, payload


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary')
    parser.add_argument('--evidence')
    arguments = parser.parse_args()
    admin, credentials = require_environment()
    binary = str(pathlib.Path(arguments.binary).resolve())
    os.environ['TZ'] = 'Asia/Seoul'
    time.tzset()
    version = verify_database(admin)
    reset_tables(admin)
    start, end, yesterday, today = seed_tables(admin)
    evidence = {'database_version': version, 'scope': 'isolated guarded MariaDB fixtures only', 'checks': []}
    with tempfile.TemporaryDirectory(prefix='resource-monitor-log-volume-db-') as temporary:
        root = pathlib.Path(temporary)
        output = root / 'monitor'
        output.mkdir()
        config = root / 'settings.json'
        log = root / 'daemon.log'
        settings = {
            'schema_version': 1,
            'is_analyzer': True,
            'collection': {'interval_seconds': 600, 'output_directory': str(output)},
            'retention': {'enabled': False, 'max_age_seconds': 3600},
            'process_monitor': {'analyzer': [], 'collector': []},
            'thresholds': {'cpu_usage_percent': 100, 'memory_usage_percent': 100, 'storage_usage_percent': 100},
            'log_volume': {'mode': 'interval', 'interval_seconds': 300, 'daily_at': '00:10'},
            'mysql': {'host': 'window-db', 'port': 3306, 'database': 'CloudESM',
                      'credentials_file': str(credentials), 'tls_mode': 'disabled',
                      'connect_timeout_seconds': 1, 'read_timeout_seconds': 1, 'write_timeout_seconds': 1},
        }
        write_settings(config, settings)
        seen = set()
        started = time.monotonic()
        with daemon(binary, config, log) as process:
            payload = wait_snapshot(output, seen, lambda item: item['log_volume']['status'] == 'ok', process, log)
            elapsed = time.monotonic() - started
            assert elapsed < 10, elapsed
            assert_volume(payload, 'interval', {'192.0.2.10': 31, '192.0.2.20': 13, '2001:db8::1': 0})
            assert payload['log_volume']['window_start'] == local_stamp(start), payload['log_volume']
            assert payload['log_volume']['window_end_exclusive'] == local_stamp(end), payload['log_volume']
            evidence['checks'].append({'name': 'interval_bounds_category_sum_independent_600s_collection', 'elapsed_seconds': elapsed, 'snapshot': payload})
            local = datetime.datetime.now()
            daily_at = (local - datetime.timedelta(minutes=1)).strftime('%H:%M') if local.hour or local.minute else '00:00'
            settings['log_volume']['mode'] = 'daily'
            settings['log_volume']['daily_at'] = daily_at
            write_settings(config, settings)
            payload = wait_snapshot(output, seen, lambda item: item['log_volume']['mode'] == 'daily' and item['log_volume']['status'] == 'ok', process, log)
            assert_volume(payload, 'daily', {'192.0.2.10': 300, '192.0.2.20': 40})
            assert payload['log_volume']['window_start'][:10] == yesterday.isoformat(), payload['log_volume']
            assert payload['log_volume']['window_end_exclusive'][:10] == today.isoformat(), payload['log_volume']
            evidence['checks'].append({'name': 'daily_previous_complete_day_reload', 'snapshot': payload})
            sql(admin, 'TRUNCATE TABLE CloudESM.SUM_COLLECT_INFO;')
            settings['log_volume']['mode'] = 'interval'
            settings['log_volume']['interval_seconds'] = 60
            write_settings(config, settings)
            payload = wait_snapshot(output, seen, lambda item: item['log_volume']['mode'] == 'interval' and item['log_volume']['status'] == 'no_data', process, log)
            assert_volume(payload, 'interval', {})
            evidence['checks'].append({'name': 'empty_interval_no_data', 'snapshot': payload})
            settings['is_analyzer'] = False
            settings['collection']['interval_seconds'] = 1
            write_settings(config, settings)
            wait_snapshot(output, seen, lambda item: item['log_volume']['status'] == 'disabled', process, log)
            settings['mysql']['port'] = 1
            settings['is_analyzer'] = True
            write_settings(config, settings)
            payload = wait_snapshot(output, seen, lambda item: item['log_volume']['status'] == 'error', process, log)
            assert payload['log_volume']['error'], payload
            assert payload['memory']['total_bytes'] > 0 and payload['cpu']['usage_percent'] is not None, payload
            followup = wait_snapshot(output, seen, lambda item: item['log_volume']['status'] == 'error', process, log)
            evidence['checks'].append({'name': 'connection_failure_preserves_repeated_resource_snapshots', 'snapshot': followup})
            settings['mysql']['port'] = 3306
            write_settings(config, settings)
            payload = wait_snapshot(output, seen, lambda item: item['log_volume']['status'] == 'no_data', process, log)
            assert_volume(payload, 'interval', {})
            evidence['checks'].append({'name': 'connection_settings_recovery_without_restart', 'snapshot': payload})
        settings['is_analyzer'] = False
        settings['mysql']['port'] = 1
        settings['mysql']['credentials_file'] = str(root / 'deliberately-missing.json')
        write_settings(config, settings)
        with daemon(binary, config, log) as process:
            payload = wait_snapshot(output, seen, lambda item: item['log_volume']['status'] == 'disabled', process, log)
            assert payload['collect_count']['status'] == 'disabled', payload
            assert payload['log_silence']['status'] == 'disabled', payload
            assert payload['log_volume']['equipment'] == [] and payload['log_volume']['error'] is None, payload
            assert not any(error['component'].startswith('mysql') for error in payload['errors']), payload
            evidence['checks'].append({'name': 'collector_disabled_with_unavailable_database_and_missing_credentials', 'snapshot': payload})
    if arguments.evidence:
        pathlib.Path(arguments.evidence).write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    print('Log volume database integration passed: interval boundaries and category SUM, independent output, daily reload, no_data, connection failure/recovery, collector disabled')


if __name__ == '__main__':
    main()
