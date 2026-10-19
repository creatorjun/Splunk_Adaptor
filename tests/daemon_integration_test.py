# tests/daemon_integration_test.py
import contextlib
import datetime
import fcntl
import json
import os
import pathlib
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time


def write_config(path, cpu=0, memory=0, storage=0, collection=None, is_analyzer=None, mysql=None):
    temporary = path.with_suffix('.new')
    settings = {
        'schema_version': 1,
        'thresholds': {
            'cpu_usage_percent': cpu,
            'memory_usage_percent': memory,
            'storage_usage_percent': storage,
        },
    }
    if collection is not None:
        settings['collection'] = collection
    if is_analyzer is not None:
        settings['is_analyzer'] = is_analyzer
    if mysql is not None:
        settings['mysql'] = mysql
    temporary.write_text(json.dumps(settings), encoding='utf-8')
    os.replace(temporary, path)


def collection(interval, output):
    return {'interval_seconds': interval, 'output_directory': str(output)}


def assert_local_timestamp(value, milliseconds=False):
    fraction = r'\.\d{3}' if milliseconds else ''
    assert re.fullmatch(r'\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}' + fraction + r'[+-]\d{2}:\d{2}', value), value
    normalized = value[:-3] + value[-2:]
    pattern = '%Y-%m-%dT%H:%M:%S' + ('.%f' if milliseconds else '') + '%z'
    parsed = datetime.datetime.strptime(normalized, pattern)
    local = time.localtime(parsed.timestamp())
    offset = time.strftime('%z', local)
    assert value[:19] == time.strftime('%Y-%m-%dT%H:%M:%S', local), value
    assert value[-6:] == offset[:3] + ':' + offset[3:], value
    return local


def assert_snapshot_timezone(path, payload):
    local = assert_local_timestamp(payload['timestamp'])
    assert_local_timestamp(payload['thresholds_applied_at'], milliseconds=True)
    basename = time.strftime('%Y%m%dT%H%M%S%z', local)
    assert re.fullmatch(re.escape(basename) + r'(?:_[1-9]\d*)?\.json', path.name), path.name


def wait_snapshot(output, seen, predicate, process, seconds=7):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        assert process.poll() is None, 'daemon exited unexpectedly'
        for path in sorted(output.glob('*.json')):
            if path.name in seen:
                continue
            payload = json.loads(path.read_text(encoding='utf-8'))
            seen.add(path.name)
            assert payload['schema_version'] == 1
            assert payload['status'] in ('ok', 'warning', 'unknown')
            assert payload['cpu']['window_seconds'] > 0
            assert 0 <= payload['memory']['usage_percent'] <= 100
            assert_snapshot_timezone(path, payload)
            if predicate(payload):
                return payload
        time.sleep(0.03)
    raise AssertionError('expected snapshot did not arrive in ' + str(output))


def wait_log(path, marker, process, offset=0, seconds=5):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        text = path.read_text(encoding='utf-8')
        if marker in text[offset:]:
            return text
        assert process.poll() is None, text
        time.sleep(0.03)
    raise AssertionError('missing log marker ' + marker + '\n' + path.read_text(encoding='utf-8'))


@contextlib.contextmanager
def running_daemon(command, log_path):
    with log_path.open('w', encoding='utf-8') as log:
        process = subprocess.Popen(command, stdout=log, stderr=log)
        try:
            wait_log(log_path, 'resource-monitor started', process)
            yield process
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=3)


def cycle_events(text):
    for line in text.splitlines():
        parts = line.split()
        if parts and parts[0] in ('snapshot_saved', 'snapshot_skipped'):
            event = dict(part.split('=', 1) for part in parts[1:])
            event['event'] = parts[0]
            yield event


def assert_cycle_action(event):
    assert event['status'] in ('ok', 'warning', 'unknown'), event
    assert event['event'] == 'snapshot_saved', event


def validate_settings(executable, config, output):
    base = [executable, '--config', str(config)]
    for fields in (None, {}, {'interval_seconds': 1}, {'output_directory': str(output)}):
        write_config(config, collection=fields)
        result = subprocess.run(base + ['--check-config'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
        assert result.returncode == 0, result.stderr
    for fields in ({'interval_seconds': 0}, {'output_directory': 123}):
        write_config(config, collection=fields)
        result = subprocess.run(base + ['--check-config'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
        assert result.returncode == 78, result.stderr
        assert 'component=settings_config' in result.stderr
    for is_analyzer in (None, False, True):
        database = {
            'database': 'monitoring',
            'tls_mode': 'disabled',
            'credentials_file': str(config.parent / 'missing-mysql.json'),
        }
        write_config(config, is_analyzer=is_analyzer, mysql=database)
        result = subprocess.run(base + ['--check-config'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
        requires_credentials = is_analyzer is True
        assert result.returncode == (78 if requires_credentials else 0), result.stderr
        if requires_credentials:
            assert 'component=mysql_credentials' in result.stderr
        for enabled in (False, True):
            database['enabled'] = enabled
            write_config(config, is_analyzer=is_analyzer, mysql=database)
            result = subprocess.run(base + ['--check-config'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
            assert result.returncode == 78, result.stderr
            assert 'component=settings_config' in result.stderr
    for database in (None, {}, {'database': '', 'tls_mode': 'disabled'}, {'database': 'monitoring'}):
        write_config(config, is_analyzer=True, mysql=database)
        result = subprocess.run(base + ['--check-config'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
        assert result.returncode == 78, result.stderr
        assert 'component=settings_config' in result.stderr
    for is_analyzer in ('true', 'false', 1, 0, [], {}):
        write_config(config, is_analyzer=is_analyzer)
        result = subprocess.run(base + ['--check-config'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
        assert result.returncode == 78, result.stderr
        assert 'component=settings_config' in result.stderr
    config.write_text('{', encoding='utf-8')
    result = subprocess.run(base + ['--once'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
    assert result.returncode == 78, result.stderr
    assert 'component=settings_config' in result.stderr
    assert not list(output.glob('*.json'))


def analyzer_reload_test(executable, root, config):
    output = root / 'analyzer-snapshots'
    output.mkdir()
    credentials = root / 'analyzer-credentials.json'
    credentials.write_text(json.dumps({'username': 'test-fixture', 'password': 'test-fixture'}), encoding='utf-8')
    credentials.chmod(0o600)
    log_path = root / 'analyzer.log'
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(2)
        listener.settimeout(0.1)
        database = {
            'host': '127.0.0.1',
            'port': listener.getsockname()[1],
            'database': 'monitoring',
            'tls_mode': 'disabled',
            'credentials_file': str(credentials),
            'connect_timeout_seconds': 1,
            'read_timeout_seconds': 1,
            'write_timeout_seconds': 1,
        }

        def assert_no_connection():
            try:
                connection, address = listener.accept()
            except socket.timeout:
                return
            connection.close()
            raise AssertionError('analyzer disabled but database connection attempted: ' + str(address))

        write_config(config, collection=collection(1, output), mysql=database)
        with running_daemon([executable, '--config', str(config)], log_path) as process:
            original_pid = process.pid
            seen = set()
            wait_snapshot(output, seen, lambda value: value['config_status'] == 'ok', process)
            wait_snapshot(output, seen, lambda value: value['config_status'] == 'ok', process)
            assert_no_connection()
            database['enabled'] = True
            write_config(config, collection=collection(1, output), is_analyzer=True, mysql=database)
            wait_snapshot(output, seen, lambda value: value['config_status'] == 'using_last_valid', process)
            assert_no_connection()
            del database['enabled']
            write_config(config, collection=collection(1, output), is_analyzer=True, mysql=database)
            deadline = time.monotonic() + 5
            connected = False
            while time.monotonic() < deadline:
                assert process.poll() is None
                try:
                    connection, _ = listener.accept()
                except socket.timeout:
                    continue
                connection.close()
                connected = True
                break
            assert connected, 'enabling analyzer did not start a database connection'
            wait_snapshot(output, seen, lambda value: value['config_status'] == 'using_last_valid', process)
            log_offset = len(log_path.read_text(encoding='utf-8'))
            database['credentials_file'] = str(root / 'missing-analyzer-credentials.json')
            write_config(config, collection=collection(1, output), is_analyzer=False, mysql=database)
            wait_log(log_path, 'settings_reloaded', process, offset=log_offset)
            wait_snapshot(output, seen, lambda value: value['config_status'] == 'ok', process)
            wait_snapshot(output, seen, lambda value: value['config_status'] == 'ok', process)
            assert_no_connection()
            assert process.pid == original_pid and process.poll() is None
            process.send_signal(signal.SIGTERM)
            assert process.wait(timeout=3) == 0


def hot_reload_test(executable, root, config, output):
    other = root / 'other-snapshots'
    other.mkdir()
    missing = root / 'created-later'
    locked = root / 'locked-snapshots'
    locked.mkdir()
    log_path = root / 'daemon.log'
    base = [executable, '--config', str(config)]
    write_config(config, collection=collection(60, output))
    with running_daemon(base, log_path) as process:
        original_pid = process.pid
        assert not list(output.glob('*.json')), 'first sample must wait for its CPU window'
        changed_at = time.monotonic()
        write_config(config, collection=collection(1, output))
        seen = set()
        first = wait_snapshot(output, seen, lambda value: value['config_status'] == 'ok', process, seconds=4)
        assert time.monotonic() - changed_at < 4, '60-second interval was not reloaded promptly'
        assert first['status'] == 'warning'
        assert first['cpu']['status'] == 'warning'
        assert first['memory']['status'] == 'warning'
        steady = wait_snapshot(output, seen, lambda value: value['config_status'] == 'ok', process)
        assert 0.7 <= steady['cpu']['window_seconds'] < 1.8
        assert steady['thresholds_applied_at'] == first['thresholds_applied_at']
        duplicate = subprocess.run(base + ['--once'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
        assert duplicate.returncode != 0 and 'lock' in duplicate.stderr, duplicate.stderr
        config.write_text('{"schema_version": 1,', encoding='utf-8')
        fallback = wait_snapshot(output, seen, lambda value: value['config_status'] == 'using_last_valid', process)
        assert fallback['applied_thresholds']['cpu_usage_percent'] == 0
        assert any(error['component'] == 'settings_config' for error in fallback['errors'])
        log_offset = len(log_path.read_text(encoding='utf-8'))
        write_config(config, collection=collection(1, output))
        wait_log(log_path, 'settings_recovered', process, offset=log_offset)
        recovered = wait_snapshot(output, seen, lambda value: value['config_status'] == 'ok', process)
        assert recovered['thresholds_applied_at'] == first['thresholds_applied_at']
        write_config(config, 100, 100, 100, collection=collection(1, output))
        high = wait_snapshot(output, seen, lambda value: value['config_status'] == 'ok' and value['applied_thresholds']['cpu_usage_percent'] == 100, process)
        assert high['thresholds_applied_at'] != first['thresholds_applied_at']
        assert 0.7 <= high['cpu']['window_seconds'] < 1.8
        write_config(config, 100, 100, 100, collection=collection(3, output))
        wait_log(log_path, 'settings_reloaded interval_seconds=3', process)
        before_path_change = wait_snapshot(output, seen, lambda value: value['cpu']['window_seconds'] >= 2.5, process)
        assert before_path_change['thresholds_applied_at'] == high['thresholds_applied_at']
        time.sleep(0.15)
        write_config(config, 100, 100, 100, collection=collection(3, other))
        other_seen = set()
        moved = wait_snapshot(other, other_seen, lambda value: value['config_status'] == 'ok', process, seconds=4)
        assert 2.5 <= moved['cpu']['window_seconds'] < 3.8, 'output-only update reset the collection timer'
        assert moved['thresholds_applied_at'] == high['thresholds_applied_at']
        output_count = len(list(output.glob('*.json')))
        with (output / '.resource-monitor.lock').open('a') as old_lock:
            fcntl.flock(old_lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.flock(old_lock.fileno(), fcntl.LOCK_UN)
        write_config(config, collection=collection(1, other))
        current = wait_snapshot(other, other_seen, lambda value: value['config_status'] == 'ok' and value['applied_thresholds']['cpu_usage_percent'] == 0, process)
        assert current['status'] == 'warning'
        write_config(config, 100, 100, 100, collection=collection(2, missing))
        rejected = wait_snapshot(other, other_seen, lambda value: value['config_status'] == 'using_last_valid', process)
        assert rejected['applied_thresholds']['cpu_usage_percent'] == 0
        assert rejected['thresholds_applied_at'] == current['thresholds_applied_at']
        assert 0.7 <= rejected['cpu']['window_seconds'] < 1.8, 'failed output change changed the interval'
        assert rejected['errors']
        assert not missing.exists()
        unchanged_config = config.read_bytes()
        missing.mkdir()
        missing_seen = set()
        retried = wait_snapshot(missing, missing_seen, lambda value: value['config_status'] == 'ok' and value['applied_thresholds']['cpu_usage_percent'] == 100, process, seconds=5)
        assert config.read_bytes() == unchanged_config, 'retry required a configuration rewrite'
        assert retried['thresholds_applied_at'] != rejected['thresholds_applied_at']
        assert retried['cpu']['window_seconds'] >= 1.7
        other_count = len(list(other.glob('*.json')))
        with (locked / '.resource-monitor.lock').open('a') as held_lock:
            fcntl.flock(held_lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            write_config(config, collection=collection(1, locked))
            blocked = wait_snapshot(missing, missing_seen, lambda value: value['config_status'] == 'using_last_valid', process)
            assert blocked['applied_thresholds']['cpu_usage_percent'] == 100
            assert blocked['thresholds_applied_at'] == retried['thresholds_applied_at']
            assert 1.7 <= blocked['cpu']['window_seconds'] < 2.8
            assert not list(locked.glob('*.json'))
            unchanged_config = config.read_bytes()
            fcntl.flock(held_lock.fileno(), fcntl.LOCK_UN)
        locked_seen = set()
        unlocked = wait_snapshot(locked, locked_seen, lambda value: value['config_status'] == 'ok' and value['applied_thresholds']['cpu_usage_percent'] == 0, process, seconds=4)
        assert config.read_bytes() == unchanged_config
        assert unlocked['status'] == 'warning'
        log_offset = len(log_path.read_text(encoding='utf-8'))
        write_config(config, collection=collection(1, str(locked) + '/.'))
        wait_log(log_path, 'output=' + str(locked) + '/.', process, offset=log_offset)
        alias = wait_snapshot(locked, locked_seen, lambda value: value['config_status'] == 'ok', process)
        assert alias['thresholds_applied_at'] == unlocked['thresholds_applied_at']
        assert 0.7 <= alias['cpu']['window_seconds'] < 1.8
        write_config(config, collection=collection(1, output))
        returned = wait_snapshot(output, seen, lambda value: value['config_status'] == 'ok' and value['applied_thresholds']['cpu_usage_percent'] == 0, process)
        assert returned['status'] == 'warning'
        assert len(list(output.glob('*.json'))) == output_count + 1
        assert len(list(other.glob('*.json'))) == other_count
        assert process.pid == original_pid and process.poll() is None
        for directory in (output, other, missing, locked):
            assert not list(directory.glob('*.tmp'))
        started_stop = time.monotonic()
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=3) == 0
        assert time.monotonic() - started_stop < 3
        count = len(list(output.glob('*.json')))
        time.sleep(0.15)
        assert len(list(output.glob('*.json'))) == count
    log_text = log_path.read_text(encoding='utf-8')
    for event in cycle_events(log_text):
        assert_cycle_action(event)
    assert log_text.count('resource-monitor started') == 1
    assert 'snapshot_skipped' not in log_text


def override_and_once_tests(executable, root, config):
    interval_output = root / 'interval-override-first'
    interval_other = root / 'interval-override-next'
    output_override = root / 'output-override'
    ignored_output = root / 'ignored-output'
    once_output = root / 'once-output'
    for directory in (interval_output, interval_other, output_override, ignored_output, once_output):
        directory.mkdir()
    write_config(config, collection=collection(60, interval_output))
    interval_command = [executable, '--interval-seconds', '1', '--config', str(config)]
    with running_daemon(interval_command, root / 'interval-override.log') as process:
        wait_snapshot(interval_output, set(), lambda value: value['config_status'] == 'ok', process)
        write_config(config, 100, 100, 100, collection=collection(30, interval_other))
        changed = wait_snapshot(interval_other, set(), lambda value: value['applied_thresholds']['cpu_usage_percent'] == 100, process, seconds=3)
        assert 0.7 <= changed['cpu']['window_seconds'] < 1.8
    write_config(config, collection=collection(60, ignored_output))
    output_command = [executable, '--output', str(output_override), '--config', str(config)]
    with running_daemon(output_command, root / 'output-override.log') as process:
        write_config(config, 100, 100, 100, collection=collection(1, interval_other))
        changed = wait_snapshot(output_override, set(), lambda value: value['applied_thresholds']['cpu_usage_percent'] == 100, process, seconds=4)
        assert changed['config_status'] == 'ok'
        assert not list(ignored_output.glob('*.json'))
    write_config(config, collection=collection(60, once_output))
    once_command = [executable, '--config', str(config), '--once']
    once_log = root / 'once.log'
    with running_daemon(once_command, once_log) as process:
        write_config(config, collection=collection(1, once_output))
        assert process.wait(timeout=4) == 0, once_log.read_text(encoding='utf-8')
    once_files = list(once_output.glob('*.json'))
    assert len(once_files) == 1
    once_payload = json.loads(once_files[0].read_text(encoding='utf-8'))
    assert once_payload['status'] == 'warning'
    assert_snapshot_timezone(once_files[0], once_payload)
    events = list(cycle_events(once_log.read_text(encoding='utf-8')))
    assert len(events) == 1
    assert_cycle_action(events[0])
    legacy_config = root / 'thresholds.json'
    write_config(legacy_config, 100, 100, 100)
    overridden = subprocess.run([executable, '--interval-seconds', '1', '--output', str(once_output), '--config', str(legacy_config), '--once'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
    assert overridden.returncode == 0, overridden.stderr
    assert 'interval_seconds=1' in overridden.stdout
    assert 'output=' + str(once_output) in overridden.stdout
    assert len(list(once_output.glob('*.json'))) == 2
    for path in once_output.glob('*.json'):
        assert_snapshot_timezone(path, json.loads(path.read_text(encoding='utf-8')))
    events = list(cycle_events(overridden.stdout))
    assert len(events) == 1
    assert_cycle_action(events[0])


def main():
    time.tzset()
    executable = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='resource-monitor-integration-') as temporary:
        root = pathlib.Path(temporary)
        output = root / 'snapshots'
        output.mkdir()
        config = root / 'settings.json'
        validate_settings(executable, config, output)
        analyzer_reload_test(executable, root, config)
        hot_reload_test(executable, root, config, output)
        override_and_once_tests(executable, root, config)
    print('Settings validation, analyzer database connection gating and live reload, OS timezone timestamps and filenames, atomic live interval/output/threshold reload, retry, aliases, CLI overrides, legacy config, once saves and SIGTERM passed')


if __name__ == '__main__':
    main()
