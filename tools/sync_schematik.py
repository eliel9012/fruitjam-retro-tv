#!/usr/bin/env python3
"""Keep Schematik's embedded source in sync with the source actually compiled.

Only the source copy, the library list and the build settings come from the
tree. The hardware description (board, pins, peripherals, assembly steps) lives
in schematik-project.json itself and is edited by hand when the hardware changes.
"""
import argparse
import configparser
import json
from pathlib import Path

ENV = 'env:fruitjam'


def library_entry(line):
    """'owner/name@1.2.3' ou 'https://.../name.git#commit' -> {'name', 'version'}."""
    if '://' in line:
        url, _, ref = line.partition('#')
        name = url.rstrip('/').split('/')[-1].removesuffix('.git')
        # Commit completo polui a interface; os 12 primeiros bastam para achar.
        version = f'git#{ref[:12]}' if ref else 'git'
    elif '@' in line:
        name, version = line.split('@', 1)
        name = name.split('/')[-1]
    else:
        name, version = line.split('/')[-1], 'latest'
    return {'name': name, 'version': version}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    path = root / 'schematik-project.json'
    original = json.loads(path.read_text())
    project = json.loads(json.dumps(original))
    # Arquivo que saiu da árvore sai do pacote também: senão o Schematik
    # continuaria compilando uma cópia de algo que o firmware já não usa.
    existing = {f['path']: f for f in project['projectPackage']['files'] if (root / f['path']).is_file()}
    sources = [*root.glob('src/*.cpp'), *root.glob('src/fj/*.cpp'),
               *root.glob('include/*.h'), *root.glob('include/fj/*.h'), root / 'platformio.ini']
    for file in sorted(sources):
        name = file.relative_to(root).as_posix()
        existing[name] = {**existing.get(name, {}), 'path': name, 'content': file.read_text()}
    project['projectPackage']['files'] = list(existing.values())
    project['code']['code'] = (root / 'src/main.cpp').read_text()
    config = configparser.ConfigParser()
    config.read(root / 'platformio.ini')
    env = config[ENV]
    project['code']['libraries'] = [
        library_entry(line.strip()) for line in env['lib_deps'].splitlines() if line.strip()
    ]
    build = project['code']['buildSettings']
    build['platformioPlatform'] = env['platform']
    build['platformioBoardId'] = env['board']
    build['extraBuildFlags'] = env['build_flags'].split()
    if args.check:
        if project != original:
            raise SystemExit('Schematik is out of sync; run python3 tools/sync_schematik.py')
        print('PASS: Schematik sources and dependencies match the compiled project')
    else:
        path.write_text(json.dumps(project, ensure_ascii=False, indent=2) + '\n')
        print('Schematik synchronized')


if __name__ == '__main__':
    main()
