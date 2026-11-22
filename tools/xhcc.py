"""Desktop host: select a configuration package and run the shared xhcc Wasm."""
from pathlib import Path, PurePosixPath
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

BASE_FIELDS = {'version', 'id', 'tools', 'target', 'cpu', 'abi', 'include'}


def manifest(path):
    data = {}
    for raw in path.read_text(encoding='utf-8').splitlines():
        line = raw.strip()
        if not line or line.startswith(('#', ';')):
            continue
        if '=' not in line:
            raise ValueError(f'invalid manifest: {path}')
        key, value = (s.strip() for s in line.split('=', 1))
        if not key or key in data:
            raise ValueError(f'duplicate/empty key {key}: {path}')
        data[key] = value
    if data.get('version') == '2':
        raise ValueError(f'version 2 staged packages are obsolete; upgrade to version 3: {path}')
    if not BASE_FIELDS <= data.keys() or data['version'] not in ('1', '3') or data['id'] != path.parent.name:
        raise ValueError(f'invalid package schema: {path}')
    allowed = BASE_FIELDS | ({'module'} if data['version'] == '1' else {'dialect', 'options', 'engine', 'encoding', 'selection', 'templates', 'object', 'relocations', 'requires'})
    if data.keys() - allowed:
        raise ValueError(f'unsupported package fields: {path}')
    if 'requires' in data:
        capabilities = data['requires'].split(',')
        if len(set(capabilities)) != len(capabilities) or any(
                re.fullmatch(r'[a-z][a-z0-9]*(?:[._-][a-z0-9]+)*', item) is None for item in capabilities):
            raise ValueError(f'invalid requires capability list: {path}')
    if data.get('dialect', 'gcc') not in ('gcc', 'armcc'):
        raise ValueError(f'unsupported dialect: {path}')
    for key in ('include', *[key for key in ('options', 'encoding', 'selection', 'templates', 'object', 'relocations') if key in data]):
        value = data[key]
        p = PurePosixPath(value)
        if not value or '\\' in value or ':' in value or p.is_absolute() or any(x in ('', '..', '.') for x in value.split('/')):
            raise ValueError(f'invalid {key} path: {path}')
        if not (path.parent / value).resolve().is_relative_to(path.parent.resolve()):
            raise ValueError(f'{key} escapes package: {path}')
    # Legacy module is inert metadata: never resolve, stat, open or execute it.
    return data


def discover(root):
    tools = {}
    for path in sorted(root.glob('*/package.ini')):
        data = manifest(path)
        for name in data['tools'].split(','):
            if not name or name.strip() != name or any(c.isspace() or c in '/\\:' for c in name) or name.startswith('-') or name in tools:
                raise ValueError(f'duplicate/invalid tool alias: {name}')
            tools[name] = (path.parent, data)
    return tools


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--package-root', type=Path, default=Path(__file__).resolve().parents[1] / 'packages')
    p.add_argument('--compiler', type=Path, default=Path(__file__).resolve().parents[1] / 'build/wasm/xhcc.wasm')
    p.add_argument('--wasmtime', default=os.environ.get('WASMTIME') or shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
    p.add_argument('--list-tools', '--list', action='store_true')
    p.add_argument('--dry-run', action='store_true')
    p.add_argument('--json', action='store_true')
    p.add_argument('--trace-load', action='store_true')
    p.add_argument('tool', nargs='?')
    p.add_argument('arguments', nargs=argparse.REMAINDER)
    a = p.parse_args(argv)
    root = a.package_root.resolve()
    if not root.is_dir():
        p.error(f'package root missing: {root}')
    tools = discover(root)
    if a.list_tools:
        if a.json:
            print(json.dumps({name: data['id'] for name, (_, data) in tools.items()}))
        else:
            for name, (_, data) in tools.items():
                print(f'{name}: {data["id"]} ({data["target"]}/{data["cpu"]}/{data["abi"]})')
        return 0
    if a.tool not in tools:
        p.error('specify one installed tool; use --list-tools')
    directory, data = tools[a.tool]
    compiler = a.compiler.resolve()
    if not compiler.is_file():
        p.error(f'shared compiler module missing: {compiler}; build/install xhcc first')
    environment = {'XHCC_PACKAGE_ROOT': '/xh-packages', 'XHCC_PACKAGE_DIR': '/xh-packages/' + data['id']}
    command = [a.wasmtime, 'run', '--dir=.', '--dir=' + str(root) + '::/xh-packages']
    for key, value in environment.items():
        command += ['--env', key + '=' + value]
    command += [str(compiler), a.tool, *a.arguments]
    plan = {'package': data['id'], 'module': str(compiler), 'argv': [a.tool, *a.arguments],
            'environment': environment, 'command': command, 'dry_run': a.dry_run,
            'worker_count': 0 if a.dry_run else 1}
    if a.trace_load:
        print(json.dumps({'event': 'plan' if a.dry_run else 'execute', 'module': str(compiler),
                          'package': data['id'], 'worker_count': plan['worker_count']}), file=sys.stderr, flush=True)
    if a.json:
        print(json.dumps(plan), flush=True)
    elif a.dry_run:
        print(json.dumps(command))
    if a.dry_run:
        return 0
    return subprocess.run(command).returncode


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f'xhcc host: {error}', file=sys.stderr)
        sys.exit(1)
