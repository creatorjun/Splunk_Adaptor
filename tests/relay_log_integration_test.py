# tests/relay_log_integration_test.py
import contextlib
import json
import pathlib
import signal
import subprocess
import sys
import tempfile
import time


def error_line(marker, suffix='', logger=False):
    prefix = '2026-10-29 12:34:56.789 ERROR '
    if logger:
        prefix += '[relay-worker] CollectorForAgent - '
    return prefix + 'marker=' + marker + ' eqp_ip=192.0.2.1' + suffix + '\n'


def append(path, contents):
    with path.open('a', encoding='utf-8', newline='') as stream:
        stream.write(contents)


def write_settings(path, settings):
    replacement = path.with_suffix('.new')
    replacement.write_text(json.dumps(settings), encoding='utf-8')
    replacement.replace(path)


@contextlib.contextmanager
def daemon(binary, config, log):
    with log.open('w', encoding='utf-8') as stream:
        process = subprocess.Popen([binary, '--config', str(config)], stdout=stream, stderr=stream)
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


def next_snapshot(output, seen, process, log, predicate=lambda item: True, timeout=10):
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        assert process.poll() is None, log.read_text(encoding='utf-8')
        for path in sorted(output.glob('*.json')):
            if path.name in seen:
                continue
            payload = json.loads(path.read_text(encoding='utf-8'))
            seen.add(path.name)
            latest = payload
            assert payload['config_status'] == 'ok', payload
            assert payload['process_profile'] == payload['role'], payload
            assert payload['processes'] == {}, payload
            for component in ('collect_count', 'log_silence', 'log_volume'):
                assert payload[component]['status'] == 'disabled', payload
            relay = payload['relay_log']
            assert relay['error_count'] == len(relay['events']), relay
            assert isinstance(relay['backlog'], bool) and isinstance(relay['errors'], list), relay
            if payload['role'] == 'relay':
                assert 'processing_files' not in payload, payload
                assert relay['status'] in ('ok', 'warning', 'error', 'partial'), relay
                assert relay['checked_at'], relay
                if relay['events']:
                    assert relay['status'] == 'warning' and payload['status'] == 'warning', payload
            else:
                assert relay['status'] == 'disabled' and not relay['events'], relay
            if predicate(payload):
                return payload
        time.sleep(0.05)
    raise AssertionError('Expected snapshot was not observed: %r\n%s' % (latest, log.read_text(encoding='utf-8')))


def assert_event(event, line, truncated=False):
    original = line.rstrip('\r\n')
    assert event['timestamp'] == original[:23], event
    assert event['truncated'] is truncated, event
    if truncated:
        assert original.startswith(event['line']) and len(event['line']) <= 16384, event
    else:
        assert event['line'] == original, event
        message = original.split(' - ', 1)[1] if ' - ' in original else original.split(' ERROR ', 1)[1]
        assert event['message'] == message, event


def expect_lines(output, seen, process, log, lines):
    expected = {line.rstrip('\r\n'): line for line in lines}
    found = set()
    deadline = time.monotonic() + 10
    while found != set(expected):
        payload = next_snapshot(output, seen, process, log, timeout=max(0.1, deadline - time.monotonic()))
        for event in payload['relay_log']['events']:
            raw = event['line']
            assert raw in expected and raw not in found, payload
            assert_event(event, expected[raw])
            found.add(raw)
        assert time.monotonic() < deadline or found == set(expected), payload
    return payload


def expect_empty(output, seen, process, log):
    payload = next_snapshot(output, seen, process, log)
    relay = payload['relay_log']
    assert relay['status'] == 'ok' and relay['error_count'] == 0, payload
    assert relay['events'] == [] and relay['errors'] == [] and not relay['backlog'], relay
    return payload


def main():
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    evidence_path = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
    evidence = {}
    with tempfile.TemporaryDirectory(prefix='resource-monitor-relay-integration-') as temporary:
        root = pathlib.Path(temporary)
        output = root / 'monitor'
        output.mkdir()
        relay_path = root / 'CollectorForAgent.log'
        relay_path.write_text(error_line('historical-startup') + 'historical padding ' + 'x' * 1000 + '\n', encoding='utf-8')
        config = root / 'settings.json'
        log = root / 'daemon.log'
        settings = {
            'schema_version': 1,
            'role': 'relay',
            'collection': {'interval_seconds': 1, 'output_directory': str(output)},
            'retention': {'enabled': False, 'max_age_seconds': 3600},
            'process_monitor': {'analyzer': [], 'collector': [], 'relay': []},
            'thresholds': {'cpu_usage_percent': 100, 'memory_usage_percent': 100, 'storage_usage_percent': 100},
            'relay_log': {'path': str(relay_path)},
            'mysql': {'host': '127.0.0.1', 'port': 1, 'database': 'deliberately-unavailable',
                      'credentials_file': str(root / 'missing-mysql.json'), 'tls_mode': 'disabled'},
        }
        write_settings(config, settings)
        seen = set()
        with daemon(binary, config, log) as process:
            payload = expect_empty(output, seen, process, log)
            assert payload['role'] == 'relay' and payload['relay_log']['source'] == str(relay_path), payload
            evidence['initial_baseline'] = payload
            first = error_line('first-new-error',
                               ' java.net.ConnectException: Connection refused for http://192.0.2.1:8080/relay',
                               logger=True)
            append(relay_path, '2026-10-29 12:34:56.789 INFO error and ERROR in message only\n'
                   '2026-10-29 12:34:56.789 WARN connection warning\n'
                   '2026-10-29 12:34:56.789 ERRORS invalid level\n'
                   'ERROR missing timestamp\n'
                   '\tat relay.example.StackTrace.run(StackTrace.java:10)\n' + first)
            evidence['new_error_warning'] = expect_lines(output, seen, process, log, [first])
            expect_empty(output, seen, process, log)
            partial = error_line('partial-error')
            append(relay_path, partial.rstrip('\n'))
            payload = next_snapshot(output, seen, process, log)
            assert payload['relay_log']['error_count'] == 0 and payload['relay_log']['events'] == [], payload
            append(relay_path, '\n')
            expect_lines(output, seen, process, log, [partial])
            expect_empty(output, seen, process, log)
            replacement = root / 'replacement.log'
            replacement.write_text(error_line('historical-path') + 'old padding ' + 'x' * 1000 + '\n'
                                   + error_line('historical-partial').rstrip('\n'), encoding='utf-8')
            settings['relay_log']['path'] = str(replacement)
            write_settings(config, settings)
            payload = next_snapshot(output, seen, process, log,
                                    lambda item: item['relay_log']['source'] == str(replacement))
            assert payload['relay_log']['error_count'] == 0, payload
            switched = error_line('new-path-error')
            append(replacement, ' continuation must be ignored\n' + switched)
            expect_lines(output, seen, process, log, [switched])
            expect_empty(output, seen, process, log)
            copied = error_line('copytruncate-error')
            replacement.write_text(copied, encoding='utf-8')
            expect_lines(output, seen, process, log, [copied])
            expect_empty(output, seen, process, log)
            rotated = root / 'replacement.log.1'
            replacement.rename(rotated)
            old_tail = error_line('rotated-old-tail')
            append(rotated, old_tail)
            new_head = error_line('rotated-new-head')
            replacement.write_text(new_head, encoding='utf-8')
            expect_lines(output, seen, process, log, [old_tail, new_head])
            expect_empty(output, seen, process, log)
            oversized = error_line('oversized-error', 'x' * 20000)
            append(replacement, oversized)
            payload = next_snapshot(output, seen, process, log,
                                    lambda item: item['relay_log']['error_count'] != 0)
            assert payload['relay_log']['error_count'] == 1, payload
            assert_event(payload['relay_log']['events'][0], oversized, truncated=True)
            expect_empty(output, seen, process, log)
            missing = root / 'created-later.log'
            settings['relay_log']['path'] = str(missing)
            write_settings(config, settings)
            payload = next_snapshot(output, seen, process, log,
                                    lambda item: item['relay_log']['source'] == str(missing)
                                    and item['relay_log']['status'] == 'error')
            assert payload['relay_log']['errors'] and not payload['relay_log']['events'], payload
            assert payload['memory']['total_bytes'] > 0, payload
            evidence['missing_file_error'] = payload
            created = error_line('created-after-missing')
            missing.write_text(created, encoding='utf-8')
            expect_lines(output, seen, process, log, [created])
            expect_empty(output, seen, process, log)
            settings['role'] = 'collector'
            write_settings(config, settings)
            evidence['collector_disabled'] = next_snapshot(output, seen, process, log,
                                                          lambda item: item['role'] == 'collector')
            append(missing, error_line('inactive-role-history'))
            next_snapshot(output, seen, process, log, lambda item: item['role'] == 'collector')
            settings['role'] = 'relay'
            write_settings(config, settings)
            payload = next_snapshot(output, seen, process, log, lambda item: item['role'] == 'relay')
            assert payload['relay_log']['status'] == 'ok' and payload['relay_log']['events'] == [], payload
            evidence['reenabled_baseline'] = payload
            reenabled = error_line('reenabled-fresh-error')
            append(missing, reenabled)
            expect_lines(output, seen, process, log, [reenabled])
            expect_empty(output, seen, process, log)
    if evidence_path is not None:
        evidence_path.parent.mkdir(parents=True, exist_ok=True)
        evidence_path.write_text(json.dumps(evidence, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print('Relay integration passed: startup EOF, exact ERROR filtering, acknowledged events without duplicates, partial lines, path/role reload, missing/create, copytruncate, old/new rotation tails, bounded long lines')


if __name__ == '__main__':
    main()
