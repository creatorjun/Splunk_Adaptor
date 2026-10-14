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
            assert payload['cpu']['window_seconds'] > 0
            assert 0 <= payload['memory']['usage_percent'] <= 100
            assert payload['timestamp'].endswith('Z')
            if predicate(payload):
                return payload
        time.sleep(0.03)
    raise AssertionError('expected snapshot did not arrive')


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
        with (root / 'daemon.log').open('w+', encoding='utf-8') as log:
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
                write_config(config, 100, 100, 100)
                recovered = wait_snapshot(output, seen, lambda value: value['config_status'] == 'ok' and value['applied_thresholds']['cpu_usage_percent'] == 100, process)
                assert recovered['memory']['status'] == 'ok'
                assert recovered['thresholds_applied_at'] != first['thresholds_applied_at']
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
        once = subprocess.run(base + ['--once', '--interval-seconds', '1'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
        assert once.returncode == 0, once.stderr
        assert len(list(output.glob('*.json'))) == count + 1
    print('Daemon startup, CPU window, lock, reload, invalid config recovery, atomic reads and SIGTERM passed')


if __name__ == '__main__':
    main()
