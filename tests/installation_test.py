# tests/installation_test.py
import json
import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    cmake, build_directory = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='resource-monitor-install-') as temporary:
        root = pathlib.Path(temporary)
        environment = os.environ.copy()
        environment['DESTDIR'] = temporary
        command = [cmake, '--install', build_directory, '--prefix', '/usr']
        subprocess.run(command, check=True, env=environment, stdout=subprocess.PIPE)
        config = root / 'etc/resource-monitor/thresholds.json'
        original = json.loads(config.read_text())
        original['thresholds']['cpu_usage_percent'] = 42
        config.write_text(json.dumps(original), encoding='utf-8')
        subprocess.run(command, check=True, env=environment, stdout=subprocess.PIPE)
        assert json.loads(config.read_text())['thresholds']['cpu_usage_percent'] == 42
        assert (root / 'usr/sbin/resource-monitor').is_file()
        assert (root / 'usr/lib/systemd/system/resource-monitor.service').is_file()
        assert (root / 'usr/share/doc/resource-monitor/THIRD_PARTY_NOTICES.txt').is_file()
        assert json.loads((root / 'usr/share/resource-monitor/thresholds.json').read_text())['thresholds']['cpu_usage_percent'] == 80
        assert len(list(root.rglob('thresholds.json'))) == 2
    print('Staged installation paths and existing threshold preservation passed')


if __name__ == '__main__':
    main()
