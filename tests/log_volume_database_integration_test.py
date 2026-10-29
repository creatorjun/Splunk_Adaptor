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
''')


def bucket(timestamp):
    return time.strftime('%Y%m%d%H%M', time.localtime(timestamp))


def seed_interval(admin, start, end):
    values = [
        (start - 60, '192.0.2.10', 'outside-before', 10000),
        (start, '192.0.2.10', 'category-a', 7),
        (start + 60, '192.0.2.10', 'category-b', 11),
        (end - 60, '192.0.2.10', 'category-a', 13),
        (end, '192.0.2.10', 'next-interval', 20000),
        (start + 60, '192.0.2.20', 'category-a', 5),
        (end - 60, '192.0.2.20', 'category-b', 8),
        (start, '2001:db8::1', 'category-zero', 0),
    ]
    rows = ','.join("(%s,'%s','%s',%d)" % (bucket(stamp), address, category, count)
                    for stamp, address, category, count in values)
    sql(admin, 'INSERT INTO CloudESM.SUM_COLLECT_INFO (recv_time,eqp_ip,log_cate_cd,`count`) VALUES ' + rows + ';')


def write_json(path, value):
    replacement = path.with_suffix('.new')
    replacement.write_text(json.dumps(value), encoding='utf-8')
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


def wait_snapshot(output, seen, predicate, process, log, timeout=12):
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
            assert 'collect_count' not in payload and 'log_silence' not in payload, payload
            assert 'equipment' not in payload['log_volume'], payload
            assert payload['memory']['total_bytes'] > 0 and payload['cpu']['usage_percent'] is not None, payload
            if predicate(payload):
                return payload
        time.sleep(0.05)
    raise AssertionError('Expected snapshot not observed: %r\n%s' % (last, log.read_text(encoding='utf-8')))


def wait_file(path, process, log, timeout=12):
    deadline = time.monotonic() + timeout
    while not path.is_file():
        assert process.poll() is None, log.read_text(encoding='utf-8')
        assert time.monotonic() < deadline, log.read_text(encoding='utf-8')
        time.sleep(0.05)
    return json.loads(path.read_text(encoding='utf-8'))


def counts(equipment):
    return {item['eqp_ip']: item['count'] for item in equipment}


def report_name(start):
    return 'log_cnt_' + time.strftime('%Y%m%dT%H%M%S%z', time.localtime(start)) + '.json'


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
    while 300 - time.time() % 300 < 45:
        time.sleep(0.2)
    end = int(time.time()) // 300 * 300
    start = end - 300
    reset_tables(admin)
    seed_interval(admin, start, end)
    evidence = {'database_version': version, 'scope': 'isolated guarded MariaDB fixtures; simulated persisted cursor for hourly boundary, real daemon and SQL', 'checks': []}
    with tempfile.TemporaryDirectory(prefix='resource-monitor-hourly-db-') as temporary:
        root = pathlib.Path(temporary)
        output = root / 'monitor'
        output.mkdir()
        config = root / 'settings.json'
        log = root / 'daemon.log'
        settings = {
            'schema_version': 1,
            'role': 'anal',
            'collection': {'interval_seconds': 1, 'output_directory': str(output)},
            'retention': {'enabled': False, 'max_age_seconds': 3600},
            'process_monitor': {'anal': [], 'col': [], 'prx': []},
            'thresholds': {'cpu_usage_percent': 100, 'memory_usage_percent': 100, 'storage_usage_percent': 100},
            'mysql': {'host': 'window-db', 'port': 3306, 'database': 'CloudESM',
                      'credentials_file': str(credentials), 'tls_mode': 'disabled',
                      'connect_timeout_seconds': 1, 'read_timeout_seconds': 1, 'write_timeout_seconds': 1},
        }
        write_json(config, settings)
        checkpoint = output / 'log_counts/.checkpoint.json'
        seen = set()
        with daemon(binary, config, log) as process:
            payload = wait_snapshot(output, seen, lambda item: item['log_volume']['status'] == 'ok', process, log)
            state = wait_file(checkpoint, process, log)
            assert state['next_start_epoch'] == end, state
            if state['hour']:
                equipment = state['hour']['equipment']
                assert state['hour']['completed_intervals'] == 1, state
            else:
                equipment = wait_file(output / 'log_counts' / report_name(start - 3300), process, log)['equipment']
            assert counts(equipment) == {'192.0.2.10': 31, '192.0.2.20': 13, '2001:db8::1': 0}, state
            original_checkpoint = checkpoint.read_bytes()
            wait_snapshot(output, seen, lambda item: item['log_volume']['status'] == 'ok', process, log)
            assert checkpoint.read_bytes() == original_checkpoint
            evidence['checks'].append({'name': 'five_minute_bounds_category_sum_and_no_repeat', 'snapshot': payload, 'checkpoint': state})

        hour_end = end - int(time.localtime(end).tm_min) * 60
        hour_start = hour_end - 3600
        reset_tables(admin)
        seed_interval(admin, hour_end - 300, hour_end)
        state['next_start_epoch'] = hour_end - 300
        state['last_saved_hour_epoch'] = None
        state['hour'] = {'start_epoch': hour_start, 'end_epoch': hour_end,
                         'coverage_start_epoch': hour_start, 'completed_intervals': 11,
                         'equipment': [{'eqp_ip': '192.0.2.10', 'count': 100},
                                       {'eqp_ip': '192.0.2.20', 'count': 200}]}
        report = output / 'log_counts' / report_name(hour_start)
        if report.exists():
            report.unlink()
        write_json(checkpoint, state)
        with daemon(binary, config, log) as process:
            hourly = wait_file(report, process, log)
            assert hourly['kind'] == 'resource-monitor-log-counts', hourly
            assert hourly['source'] == 'CloudESM.SUM_COLLECT_INFO', hourly
            assert hourly['window']['start_epoch'] == hour_start and hourly['window']['end_epoch'] == hour_end, hourly
            assert hourly['coverage']['completed_intervals'] == 12 and hourly['coverage']['complete'] is True, hourly
            assert counts(hourly['equipment']) == {'192.0.2.10': 131, '192.0.2.20': 213, '2001:db8::1': 0}, hourly
            payload = wait_snapshot(output, seen, lambda item: item['log_volume']['status'] == 'ok', process, log)
            saved_state = json.loads(checkpoint.read_text(encoding='utf-8'))
            assert saved_state['next_start_epoch'] == end, saved_state
            if end > hour_end:
                assert counts(saved_state['hour']['equipment']) == {'192.0.2.10': 20000}, saved_state
            evidence['checks'].append({'name': 'hourly_sum_with_checkpoint_recovery_and_next_hour_separation', 'snapshot': payload, 'hourly': hourly, 'checkpoint': saved_state})
        saved_report = report.read_bytes()
        saved_checkpoint = checkpoint.read_bytes()
        sql(admin, 'UPDATE CloudESM.SUM_COLLECT_INFO SET `count`=`count`+1;')
        with daemon(binary, config, log) as process:
            wait_snapshot(output, seen, lambda item: True, process, log)
            wait_snapshot(output, seen, lambda item: True, process, log)
            assert checkpoint.read_bytes() == saved_checkpoint
            assert report.read_bytes() == saved_report
            evidence['checks'].append({'name': 'restart_no_double_count_and_no_requery_of_completed_window'})
            new_output = root / 'new-output'
            new_output.mkdir()
            settings['collection']['output_directory'] = str(new_output)
            write_json(config, settings)
            wait_snapshot(new_output, set(), lambda item: True, process, log)
            copied = wait_file(new_output / 'log_counts/.checkpoint.json', process, log)
            assert copied['next_start_epoch'] == end and copied['hour'] == saved_state['hour'], copied
            evidence['checks'].append({'name': 'output_path_reload_preserves_cursor_and_accumulator'})
            settings['role'] = 'col'
            settings['mysql']['port'] = 1
            settings['mysql']['credentials_file'] = str(root / 'deliberately-missing.json')
            write_json(config, settings)
            payload = wait_snapshot(new_output, set(), lambda item: item['role'] == 'col', process, log)
            assert payload['log_volume']['status'] == 'disabled', payload
            assert not any(error['component'].startswith('mysql') for error in payload['errors']), payload
            evidence['checks'].append({'name': 'collector_disabled_with_unavailable_database_and_missing_credentials', 'snapshot': payload})
        error_output = root / 'error-output'
        error_output.mkdir()
        settings['role'] = 'anal'
        settings['collection']['output_directory'] = str(error_output)
        settings['mysql']['credentials_file'] = str(credentials)
        write_json(config, settings)
        with daemon(binary, config, log) as process:
            payload = wait_snapshot(error_output, set(), lambda item: item['log_volume']['status'] == 'error', process, log)
            assert payload['log_volume']['error'], payload
            evidence['checks'].append({'name': 'connection_failure_keeps_resource_snapshots_and_does_not_advance_cursor', 'snapshot': payload})
            failed_state = json.loads((error_output / 'log_counts/.checkpoint.json').read_text(encoding='utf-8'))
            assert failed_state['next_start_epoch'] == start and failed_state['hour'] is None, failed_state
    if arguments.evidence:
        pathlib.Path(arguments.evidence).write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    print('Log count DB integration passed: bounded SELECT, category sums, hourly accumulation, restart deduplication, next-hour separation, reload and failure isolation')


if __name__ == '__main__':
    main()
