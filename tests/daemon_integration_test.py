# tests/daemon_integration_test.py
import json
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import time


def write_config(path, cpu=0, memory=0, storage=0, collection=None):
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
    temporary.write_text(json.dumps(settings), encoding='utf-8')
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
            assert payload['status'] in ('ok', 'warning', 'unknown')
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
    assert event['event'] == 'snapshot_saved', event


def main():
    executable = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='resource-monitor-integration-') as temporary:
        root = pathlib.Path(temporary)
        output = root / 'snapshots'
        output.mkdir()
        restarted_output = root / 'restarted-snapshots'
        restarted_output.mkdir()
        override_output = root / 'override-snapshots'
        override_output.mkdir()
        config = root / 'settings.json'
        initial_collection = {'interval_seconds': 1, 'output_directory': str(output)}
        changed_collection = {'interval_seconds': 60, 'output_directory': str(restarted_output)}
        restart_collection = {'interval_seconds': 2, 'output_directory': str(restarted_output)}
        write_config(config, collection=initial_collection)
        base = [executable, '--config', str(config)]
        check = subprocess.run(base + ['--check-config'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
        assert check.returncode == 0, check.stderr
        for collection in (None, {}, {'interval_seconds': 1}, {'output_directory': str(output)}):
            write_config(config, collection=collection)
            defaults = subprocess.run(base + ['--check-config'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
            assert defaults.returncode == 0, defaults.stderr
        for collection in ({'interval_seconds': 0}, {'output_directory': 123}):
            write_config(config, collection=collection)
            invalid_collection = subprocess.run(base + ['--check-config'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
            assert invalid_collection.returncode == 78, invalid_collection.stderr
            assert 'component=settings_config' in invalid_collection.stderr
        config.write_text('{', encoding='utf-8')
        invalid = subprocess.run(base + ['--once'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
        assert invalid.returncode == 78, invalid.stderr
        assert 'component=settings_config' in invalid.stderr
        assert not list(output.glob('*.json'))
        write_config(config, collection=initial_collection)
        log_path = root / 'daemon.log'
        with log_path.open('w+', encoding='utf-8') as log:
            process = subprocess.Popen(base, stdout=log, stderr=log)
            try:
                time.sleep(0.15)
                assert not list(output.glob('*.json')), 'first sample must wait for its CPU window'
                seen = set()
                first = wait_snapshot(output, seen, lambda value: value['config_status'] == 'ok', process)
                assert first['status'] == 'warning'
                assert first['cpu']['status'] == 'warning'
                assert first['memory']['status'] == 'warning'
                duplicate = subprocess.run(base + ['--once'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
                assert duplicate.returncode != 0 and 'lock' in duplicate.stderr
                config.write_text('{"schema_version": 1,', encoding='utf-8')
                fallback = wait_snapshot(output, seen, lambda value: value['config_status'] == 'using_last_valid', process)
                assert fallback['applied_thresholds']['cpu_usage_percent'] == 0
                assert fallback['collection_status'] == first['collection_status']
                assert fallback['errors']
                assert any(error['component'] == 'settings_config' for error in fallback['errors'])
                write_config(config, 100, 100, 100, collection=changed_collection)
                recovered = wait_snapshot(output, seen, lambda value: value['config_status'] == 'ok' and value['applied_thresholds']['cpu_usage_percent'] == 100, process)
                assert recovered['thresholds_applied_at'] != first['thresholds_applied_at']
                continuous = wait_snapshot(output, seen, lambda value: value['config_status'] == 'ok' and value['applied_thresholds']['cpu_usage_percent'] == 100, process)
                assert continuous['timestamp'] != recovered['timestamp']
                assert continuous['thresholds_applied_at'] == recovered['thresholds_applied_at']
                assert not list(restarted_output.glob('*.json')), 'live collection settings must wait for restart'
                seen.update(path.name for path in output.glob('*.json'))
                write_config(config, collection=restart_collection)
                resumed = wait_snapshot(output, seen, lambda value: value['config_status'] == 'ok' and value['applied_thresholds']['cpu_usage_percent'] == 0, process)
                assert resumed['status'] == 'warning'
                assert resumed['thresholds_applied_at'] != recovered['thresholds_applied_at']
                assert not list(restarted_output.glob('*.json')), 'live output directory changed before restart'
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
        started_restart = time.monotonic()
        restarted = subprocess.run(base + ['--once'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=8)
        assert restarted.returncode == 0, restarted.stderr
        assert time.monotonic() - started_restart >= 1.8, restarted.stdout
        assert 'interval_seconds=2' in restarted.stdout, restarted.stdout
        assert 'output=' + str(restarted_output) in restarted.stdout, restarted.stdout
        restarted_files = list(restarted_output.glob('*.json'))
        assert len(restarted_files) == 1
        assert json.loads(restarted_files[0].read_text(encoding='utf-8'))['cpu']['window_seconds'] >= 1.5
        assert len(list(output.glob('*.json'))) == count
        overridden = subprocess.run([executable, '--interval-seconds', '1', '--output', str(override_output), '--config', str(config), '--once'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
        assert overridden.returncode == 0, overridden.stderr
        assert 'interval_seconds=1' in overridden.stdout, overridden.stdout
        assert 'output=' + str(override_output) in overridden.stdout, overridden.stdout
        assert len(list(override_output.glob('*.json'))) == 1
        assert len(list(restarted_output.glob('*.json'))) == 1
        assert len(list(output.glob('*.json'))) == count
        write_config(config, 100, 100, 100, collection=initial_collection)
        once_high = subprocess.run(base + ['--once'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
        assert once_high.returncode == 0, once_high.stderr
        events = list(cycle_events(once_high.stdout))
        assert len(events) == 1, once_high.stdout
        assert_cycle_action(events[0])
        assert len(list(output.glob('*.json'))) == count + 1
        count += 1
        write_config(config, collection=initial_collection)
        once_warning = subprocess.run(base + ['--once'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
        assert once_warning.returncode == 0, once_warning.stderr
        assert 'snapshot_saved status=warning' in once_warning.stdout, once_warning.stdout
        assert len(list(output.glob('*.json'))) == count + 1
        for path in output.glob('*.json'):
            assert json.loads(path.read_text(encoding='utf-8'))['status'] in ('ok', 'warning', 'unknown')
        for event in cycle_events(log_path.read_text(encoding='utf-8')):
            assert_cycle_action(event)
        assert 'interval_seconds=1' in log_path.read_text(encoding='utf-8')
        legacy_config = root / 'thresholds.json'
        write_config(legacy_config)
        legacy = subprocess.run([executable, '--config', str(legacy_config), '--output', str(override_output), '--interval-seconds', '1', '--once'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
        assert legacy.returncode == 0, legacy.stderr
        assert len(list(override_output.glob('*.json'))) == 2
    print('Settings defaults, validation, configured collection, restart-only changes, CLI overrides, legacy config, reload, saves and SIGTERM passed')


if __name__ == '__main__':
    main()
