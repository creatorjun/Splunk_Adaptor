# tests/snapshot_retention_integration_test.py
import copy
import datetime
import json
import pathlib
import signal
import subprocess
import sys
import tempfile
import time


def write_config(path, output, interval):
    settings = {
        'schema_version': 1,
        'is_analyzer': False,
        'collection': {'interval_seconds': interval, 'output_directory': str(output)},
        'process_monitor': {'analyzer': [], 'collector': []},
        'thresholds': {'cpu_usage_percent': 100, 'memory_usage_percent': 100, 'storage_usage_percent': 100},
    }
    temporary = path.with_suffix('.new')
    temporary.write_text(json.dumps(settings), encoding='utf-8')
    temporary.replace(path)


def wait_for(predicate, process, log, timeout):
    deadline = time.monotonic() + timeout
    while not predicate():
        assert process.poll() is None, log.read_text(encoding='utf-8')
        assert time.monotonic() < deadline, log.read_text(encoding='utf-8')
        time.sleep(0.05)


def fixture(output, template, when, suffix=''):
    payload = copy.deepcopy(template)
    payload['timestamp'] = when.isoformat(timespec='seconds')
    path = output / (when.strftime('%Y%m%dT%H%M%S%z') + suffix + '.json')
    path.write_text(json.dumps(payload), encoding='utf-8')
    return path


def main():
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='resource-monitor-retention-integration-') as temporary:
        root = pathlib.Path(temporary)
        initial = root / 'initial'
        replacement = root / 'replacement'
        initial.mkdir()
        replacement.mkdir()
        config = root / 'settings.json'
        write_config(config, initial, 1)
        result = subprocess.run([binary, '--config', str(config), '--once'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                universal_newlines=True, timeout=8)
        assert result.returncode == 0, result.stderr
        template = json.loads(next(initial.glob('*.json')).read_text(encoding='utf-8'))
        now = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
        expired = fixture(initial, template, now - datetime.timedelta(hours=2))
        recent = fixture(initial, template, now - datetime.timedelta(minutes=20))
        unrelated = initial / 'settings.json'
        unrelated.write_text('{}', encoding='utf-8')
        write_config(config, initial, 86400)
        log = root / 'daemon.log'
        with log.open('w', encoding='utf-8') as stream:
            process = subprocess.Popen([binary, '--config', str(config)], stdout=stream, stderr=stream)
            try:
                wait_for(lambda: 'resource-monitor started' in log.read_text(encoding='utf-8'), process, log, 8)
                assert not expired.exists()
                assert recent.exists() and unrelated.exists()
                old_directory_fixture = fixture(initial, template, now - datetime.timedelta(hours=2), '_1')
                new_expired = fixture(replacement, template, now - datetime.timedelta(hours=2))
                new_recent = fixture(replacement, template, now - datetime.timedelta(minutes=20))
                write_config(config, replacement, 43200)
                wait_for(lambda: 'settings_reloaded interval_seconds=43200' in log.read_text(encoding='utf-8'), process, log, 8)
                periodic_started = time.monotonic()
                wait_for(lambda: not new_expired.exists(), process, log, 75)
                assert time.monotonic() - periodic_started >= 45
                assert new_recent.exists() and old_directory_fixture.exists()
                assert len(list(replacement.glob('*.json'))) == 1
                contents = log.read_text(encoding='utf-8')
                assert contents.count('snapshots_pruned count=1') == 2, contents
                assert 'snapshot_saved' not in contents, contents
                process.send_signal(signal.SIGTERM)
                assert process.wait(timeout=5) == 0
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
    print('Snapshot retention integration passed: startup, independent 60-second cleanup, output reload, preservation')


if __name__ == '__main__':
    main()
