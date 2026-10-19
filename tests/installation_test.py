# tests/installation_test.py
import json
import os
import pathlib
import stat
import subprocess
import sys
import tempfile


def main():
    cmake, build_directory = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='resource-monitor-install-') as temporary:
        root = pathlib.Path(temporary)
        command = [cmake, '--install', build_directory, '--prefix', '/usr']

        def install(stage, succeeds=True):
            environment = os.environ.copy()
            environment['DESTDIR'] = str(stage)
            result = subprocess.run(command, env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            if succeeds:
                assert result.returncode == 0, result.stderr.decode('utf-8', errors='replace')
            else:
                assert result.returncode != 0, 'installation unexpectedly accepted a non-regular configuration'

        fresh = root / 'fresh'
        install(fresh)
        config = fresh / 'etc/resource-monitor/settings.json'
        shared = fresh / 'usr/share/resource-monitor/settings.json'
        original = json.loads(config.read_text())
        assert original['collection'] == {'interval_seconds': 60, 'output_directory': '/Monitor'}
        assert original['thresholds']['cpu_usage_percent'] == 80
        assert original['is_analyzer'] is False
        assert original['log_silence'] == {'default_timeout_seconds': 300, 'overrides': []}
        assert original['processing_files'] == {'thresholds': {
            'collector': {'tmp_count': 100, 'done_count': 100},
            'normalize': {'tmp_count': 100, 'done_count': 100},
        }}
        default_profiles = {
            'analyzer': [
                'SearchEngine', 'Normalizer', 'CollectorForAgent', 'CDMEngine', 'ShovelServer',
                'AnalyzeEngine', 'AnalyzeAssistantEngine', 'SoarTicketMain', 'SoarPlaybookManager',
                'pmsServer', 'pmsClient', 'ProcessMonitor',
            ],
            'collector': [
                'SearchEngine', 'Reducer', 'Normalizer', 'CloudESMCollector', 'CollectorForAgent',
                'CDMEngine', 'pmsClient', 'ProcessMonitor',
            ],
        }
        assert original['process_monitor'] == default_profiles
        assert 'enabled' not in original['mysql']
        assert original['mysql']['credentials_file'] == '/etc/resource-monitor/secrets/mysql.json'
        assert original['mysql']['tls_mode'] == 'verify_identity'
        assert original['mysql']['tls_ca_file'] == '/etc/resource-monitor/secrets/mysql-ca.pem'
        example = fresh / 'usr/share/resource-monitor/mysql.example.json'
        assert json.loads(example.read_text()) == {
            'username': 'replace-with-database-user',
            'password': 'replace-with-database-password',
        }
        secrets = fresh / 'etc/resource-monitor/secrets'
        assert secrets.is_dir()
        assert stat.S_IMODE(secrets.stat().st_mode) == 0o750
        credentials = secrets / 'mysql.json'
        assert not credentials.exists()
        assert shared.read_bytes() == config.read_bytes()
        assert not list(fresh.rglob('thresholds.json'))
        assert len(list(fresh.rglob('settings.json'))) == 2
        original['thresholds']['cpu_usage_percent'] = 42
        original['is_analyzer'] = True
        original['log_silence'] = {'default_timeout_seconds': 600, 'overrides': []}
        original['processing_files']['thresholds']['collector']['tmp_count'] = 250
        original['process_monitor'] = {'analyzer': ['CustomAnalyzer'], 'collector': []}
        original['collection'] = {'interval_seconds': 17, 'output_directory': '/custom-monitor'}
        existing_settings = (json.dumps(original, indent=4) + '\n').encode('utf-8')
        config.write_bytes(existing_settings)
        existing_credentials = b'{"username":"installation-fixture","password":"preserve-fixture"}\n'
        credentials.write_bytes(existing_credentials)
        credentials.chmod(0o640)
        install(fresh)
        assert config.read_bytes() == existing_settings
        assert credentials.read_bytes() == existing_credentials
        assert stat.S_IMODE(credentials.stat().st_mode) == 0o640
        assert (fresh / 'usr/sbin/resource-monitor').is_file()
        assert (fresh / 'usr/lib/systemd/system/resource-monitor.service').is_file()
        assert (fresh / 'usr/share/doc/resource-monitor/THIRD_PARTY_NOTICES.txt').is_file()
        assert json.loads(shared.read_text())['thresholds']['cpu_usage_percent'] == 80
        assert json.loads(shared.read_text())['is_analyzer'] is False
        assert json.loads(shared.read_text())['log_silence'] == {'default_timeout_seconds': 300, 'overrides': []}
        assert json.loads(shared.read_text())['process_monitor'] == default_profiles
        assert 'enabled' not in json.loads(shared.read_text())['mysql']
        assert not list(fresh.rglob('thresholds.json'))

        legacy_stage = root / 'legacy'
        legacy_config = legacy_stage / 'etc/resource-monitor/thresholds.json'
        legacy_config.parent.mkdir(parents=True)
        legacy_document = {
            'schema_version': 1,
            'thresholds': {
                'cpu_usage_percent': 13.25,
                'memory_usage_percent': 71,
                'storage_usage_percent': 92,
            },
        }
        legacy_bytes = (json.dumps(legacy_document, indent=2).replace('\n', '\r\n') + '\r\n').encode('utf-8')
        legacy_config.write_bytes(legacy_bytes)
        install(legacy_stage)
        migrated = legacy_config.with_name('settings.json')
        assert migrated.read_bytes() == legacy_bytes
        assert legacy_config.read_bytes() == legacy_bytes
        assert 'collection' not in json.loads(migrated.read_text())
        assert 'is_analyzer' not in json.loads(migrated.read_text())
        assert 'process_monitor' not in json.loads(migrated.read_text())
        install(legacy_stage)
        assert migrated.read_bytes() == legacy_bytes

        both_stage = root / 'both'
        preferred = both_stage / 'etc/resource-monitor/settings.json'
        preferred.parent.mkdir(parents=True)
        preferred.write_bytes(existing_settings)
        previous = preferred.with_name('thresholds.json')
        previous.write_bytes(legacy_bytes)
        install(both_stage)
        assert preferred.read_bytes() == existing_settings
        assert previous.read_bytes() == legacy_bytes
        assert json.loads((both_stage / 'usr/share/resource-monitor/settings.json').read_text())['collection']['interval_seconds'] == 60

        malformed_stage = root / 'malformed-legacy'
        malformed_legacy = malformed_stage / 'etc/resource-monitor/thresholds.json'
        malformed_legacy.parent.mkdir(parents=True)
        malformed_bytes = b'{\r\n  "schema_version": 1,\r\n  "thresholds": unfinished\x00\xff\r\n'
        malformed_legacy.write_bytes(malformed_bytes)
        install(malformed_stage)
        assert malformed_legacy.read_bytes() == malformed_bytes
        assert malformed_legacy.with_name('settings.json').read_bytes() == malformed_bytes

        secret_link_stage = root / 'secrets-symlink'
        secret_link = secret_link_stage / 'etc/resource-monitor/secrets'
        secret_link.parent.mkdir(parents=True)
        secret_target = secret_link_stage / 'existing-credentials'
        secret_target.mkdir()
        preserved_secret = secret_target / 'mysql.json'
        preserved_secret.write_bytes(existing_credentials)
        secret_link.symlink_to(secret_target, target_is_directory=True)
        install(secret_link_stage, succeeds=False)
        assert secret_link.is_symlink()
        assert preserved_secret.read_bytes() == existing_credentials

        secret_file_stage = root / 'secrets-file'
        secret_file = secret_file_stage / 'etc/resource-monitor/secrets'
        secret_file.parent.mkdir(parents=True)
        secret_file.write_bytes(existing_credentials)
        install(secret_file_stage, succeeds=False)
        assert secret_file.read_bytes() == existing_credentials

        for filename in ('thresholds.json', 'settings.json'):
            symlink_stage = root / (filename + '-symlink')
            linked_config = symlink_stage / 'etc/resource-monitor' / filename
            linked_config.parent.mkdir(parents=True)
            target = symlink_stage / 'configuration-target.json'
            target.write_bytes(legacy_bytes)
            linked_config.symlink_to(target)
            original_link = linked_config.lstat()
            install(symlink_stage, succeeds=False)
            assert linked_config.is_symlink()
            assert linked_config.lstat().st_ino == original_link.st_ino
            assert os.readlink(linked_config) == str(target)
            assert target.read_bytes() == legacy_bytes
            if filename == 'thresholds.json':
                assert not os.path.lexists(linked_config.with_name('settings.json'))

            fifo_stage = root / (filename + '-fifo')
            fifo_config = fifo_stage / 'etc/resource-monitor' / filename
            fifo_config.parent.mkdir(parents=True)
            os.mkfifo(fifo_config)
            original_fifo = fifo_config.lstat()
            install(fifo_stage, succeeds=False)
            assert stat.S_ISFIFO(fifo_config.lstat().st_mode)
            assert fifo_config.lstat().st_ino == original_fifo.st_ino
            if filename == 'thresholds.json':
                assert not os.path.lexists(fifo_config.with_name('settings.json'))
    print('Settings and credential template installation, secret preservation, legacy migration, precedence and non-regular file rejection passed')


if __name__ == '__main__':
    main()
