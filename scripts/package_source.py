# scripts/package_source.py
import pathlib
import tarfile


root = pathlib.Path(__file__).resolve().parents[1]
destination = root / 'artifacts' / 'resource-monitor-0.9.0.tar.gz'
destination.parent.mkdir(exist_ok=True)
paths = ['src', 'tests', 'config/settings.json', 'config/mysql.example.json', 'packaging', 'scripts', 'CMakeLists.txt', 'CMakePresets.json', 'README.md']
with tarfile.open(destination, 'w:gz') as archive:
    for name in paths:
        path = root / name
        entries = sorted(path.rglob('*')) if path.is_dir() else [path]
        for entry in entries:
            excluded = '__pycache__' in entry.parts or 'secrets' in entry.parts or entry.suffix == '.pyc'
            excluded = excluded or entry.name == 'mysql.json' or entry.name == '.env' or entry.name.startswith('.env.')
            if entry.is_file() and not entry.is_symlink() and not excluded:
                relative = entry.relative_to(root).as_posix()
                archive.add(entry, arcname='resource-monitor-0.9.0/' + relative, recursive=False)
print(destination)
