#!/usr/bin/env python3
"""Keep Schematik's embedded source in sync with the source actually compiled."""
import argparse
import configparser
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    path = root / 'schematik-project.json'
    original = json.loads(path.read_text())
    project = json.loads(json.dumps(original))
    existing = {f['path']: f for f in project['projectPackage']['files']}
    for file in sorted([*root.glob('src/*.cpp'), *root.glob('include/*.h'), root / 'platformio.ini']):
        name = file.relative_to(root).as_posix()
        existing[name] = {**existing.get(name, {}), 'path': name, 'content': file.read_text()}
    project['projectPackage']['files'] = list(existing.values())
    project['code']['code'] = (root / 'src/main.cpp').read_text()
    config = configparser.ConfigParser()
    config.read(root / 'platformio.ini')
    env = config['env:m5stack-core2']
    libraries = []
    for line in env['lib_deps'].splitlines():
        if line.strip():
            name, version = line.strip().split('@')
            libraries.append({'name': name.split('/')[-1], 'version': version})
    project['code']['libraries'] = libraries
    project['code']['buildSettings']['platformioPlatform'] = env['platform']
    project['code']['buildSettings']['extraBuildFlags'] = env['build_flags'].split()
    if args.check:
        if project != original:
            raise SystemExit('Schematik is out of sync; run python3 tools/sync_schematik.py')
        print('PASS: Schematik sources and dependencies match the compiled project')
    else:
        path.write_text(json.dumps(project, ensure_ascii=False, indent=2) + '\n')
        print('Schematik synchronized')


if __name__ == '__main__':
    main()
