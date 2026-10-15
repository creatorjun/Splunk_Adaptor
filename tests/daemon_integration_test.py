# tests/daemon_integration_test.py
import json
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import time


def write_config(path, cpu=0, memory=0, storage=0):
    temporary = path.with_suffix('.new')
    temporary.write_text(json.dumps({
        'schema_version': 1,
        'thresholds': {
            'cpu_usage_percent': cpu,
            'memory_usage_percent': memory,
            'storage_usage_percent': storage,
        },
    }), encoding='utf-8')
    os.replace(temporary, path)


def wait_snapshot(output, seen, predicate, process, seconds=8):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        assert process.poll() is None, 'daemon exited unexpectedly'
        for path in sorted(output.glob('*.json')):
            if path.name in seen:
                continue
            payload = json.loads(path.read_text(encoding='utf-8'))
            seen.add(path.name)
            assert payload['schema_version'] == 1
            assert payload['status'] == 'warning', 'only warning snapshots may be written'
            assert payload['cpu']['window_seconds'] > 0
            assert 0 <= payload['memory']['usage_percent'] <= 100
            assert payload['timestamp'].endswith('Z')
            if predicate(payload):
                return payload
        time.sleep(0.03)
    raise AssertionError('expected snapshot did not arrive')


def cycle_events(text):
    for line in text.splitlines():
        parts = line.split()
        if parts and parts[0] in ('snapshot_saved', 'snapshot_skipped'):
            event = dict(part.split('=', 1) for part in parts[1:])
            event['event'] = parts[0]
            yield event


def assert_cycle_action(event):
    assert event['status'] in ('ok', 'warning', 'unknown'), event
    expected = 'snapshot_saved' if event['status'] == 'warning' else 'snapshot_skipped'
    assert event['event'] == expected, event


def wait_cycle(log_path, offset, predicate, process, seconds=8):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        assert process.poll() is None, 'daemon exited unexpectedly'
        text = log_path.read_text(encoding='utf-8')
        end = text.rfind('\n') + 1
        complete = text[offset:end] if end > offset else ''
        offset = max(offset, end)
        for event in cycle_events(complete):
            assert_cycle_action(event)
            if predicate(event):
                return event
        time.sleep(0.03)
    raise AssertionError('expected collection log did not arrive')


def main():
    executable = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='resource-monitor-integration-') as temporary:
        root = pathlib.Path(temporary)
        output = root / 'snapshots'
        output.mkdir()
        config = root / 'thresholds.json'
        write_config(config)
        base = [executable, '--config', str(config), '--output', str(output)]
        check = subprocess.run(base + ['--check-config'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
        assert check.returncode == 0, check.stderr
        config.write_text('{', encoding='utf-8')
        invalid = subprocess.run(base + ['--once', '--interval-seconds', '1'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
        assert invalid.returncode == 78, invalid.stderr
        assert not list(output.glob('*.json'))
        write_config(config)
        log_path = root / 'daemon.log'
        with log_path.open('w+', encoding='utf-8') as log:
            process = subprocess.Popen(base + ['--interval-seconds', '1'], stdout=log, stderr=log)
            try:
                time.sleep(0.15)
                assert not list(output.glob('*.json')), 'first sample must wait for its CPU window'
                seen = set()
                first = wait_snapshot(output, seen, lambda value: value['config_status'] == 'ok', process)
                assert first['status'] == 'warning'
                assert first['cpu']['status'] == 'warning'
                assert first['memory']['status'] == 'warning'
                duplicate = subprocess.run(base + ['--once', '--interval-seconds', '1'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
                assert duplicate.returncode != 0 and 'lock' in duplicate.stderr
                config.write_text('{"schema_version": 1,', encoding='utf-8')
                fallback = wait_snapshot(output, seen, lambda value: value['config_status'] == 'using_last_valid', process)
                assert fallback['applied_thresholds']['cpu_usage_percent'] == 0
                assert fallback['collection_status'] == first['collection_status']
                assert fallback['errors']
                log_offset = len(log_path.read_text(encoding='utf-8'))
                write_config(config, 100, 100, 100)
                wait_cycle(log_path, log_offset, lambda event: event['config_status'] == 'ok', process)
                seen.update(path.name for path in output.glob('*.json'))
                write_config(config)
                resumed = wait_snapshot(output, seen, lambda value: value['config_status'] == 'ok' and value['applied_thresholds']['cpu_usage_percent'] == 0, process)
                assert resumed['status'] == 'warning'
                assert resumed['thresholds_applied_at'] != first['thresholds_applied_at']
                assert not list(output.glob('*.tmp'))
                started_stop = time.monotonic()
                process.send_signal(signal.SIGTERM)
                assert process.wait(timeout=3) == 0
                assert time.monotonic() - started_stop < 3
                count = len(list(output.glob('*.json')))
                time.sleep(0.15)
                assert len(list(output.glob('*.json'))) == count
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=3)
        write_config(config, 100, 100, 100)
        once_high = subprocess.run(base + ['--once', '--interval-seconds', '1'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
        assert once_high.returncode == 0, once_high.stderr
        events = list(cycle_events(once_high.stdout))
        assert len(events) == 1, once_high.stdout
        assert_cycle_action(events[0])
        expected_additions = 1 if events[0]['status'] == 'warning' else 0
        assert len(list(output.glob('*.json'))) == count + expected_additions
        count += expected_additions
        write_config(config)
        once_warning = subprocess.run(base + ['--once', '--interval-seconds', '1'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
        assert once_warning.returncode == 0, once_warning.stderr
        assert 'snapshot_saved status=warning' in once_warning.stdout, once_warning.stdout
        assert len(list(output.glob('*.json'))) == count + 1
        for path in output.glob('*.json'):
            assert json.loads(path.read_text(encoding='utf-8'))['status'] == 'warning'
    print('Daemon startup, CPU window, lock, reload, warning-only saves, once results, atomic reads and SIGTERM passed')


if __name__ == '__main__':
    main()
