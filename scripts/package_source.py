# scripts/package_source.py
import pathlib
import tarfile


root = pathlib.Path(__file__).resolve().parents[1]
destination = root / 'artifacts' / 'resource-monitor-1.0.1.tar.gz'
destination.parent.mkdir(exist_ok=True)
paths = ['src', 'tests', 'config', 'packaging', 'scripts', 'CMakeLists.txt', 'CMakePresets.json', 'README.md']
with tarfile.open(destination, 'w:gz') as archive:
    for name in paths:
        path = root / name
        entries = sorted(path.rglob('*')) if path.is_dir() else [path]
        for entry in entries:
            if entry.is_file() and '__pycache__' not in entry.parts and entry.suffix != '.pyc':
                relative = entry.relative_to(root).as_posix()
                archive.add(entry, arcname='resource-monitor-1.0.1/' + relative, recursive=False)
print(destination)
