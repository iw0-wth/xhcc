"""Desktop host adapter: discover external packages and execute one Wasm command."""
from pathlib import Path, PurePosixPath
import argparse, os, shutil, subprocess, sys

def manifest(path):
    data={}
    for raw in path.read_text(encoding='utf-8').splitlines():
        line=raw.strip()
        if not line or line.startswith(('#',';')):continue
        if '=' not in line:raise ValueError(f'invalid manifest: {path}')
        key,value=(s.strip() for s in line.split('=',1))
        if key in data:raise ValueError(f'duplicate key {key}: {path}')
        data[key]=value
    required={'version','id','tools','target','cpu','abi','module','include'}
    if set(data)!=required or data['version']!='1' or data['id']!=path.parent.name:raise ValueError(f'invalid package schema: {path}')
    for key in ('module','include'):
        p=PurePosixPath(data[key])
        if not data[key] or '\\' in data[key] or ':' in data[key] or p.is_absolute() or any(x in ('..','.') for x in data[key].split('/')):raise ValueError(f'invalid {key} path: {path}')
        resolved=(path.parent/data[key]).resolve()
        if not resolved.is_relative_to(path.parent.resolve()):raise ValueError(f'{key} escapes package: {path}')
    return data

def discover(root):
    tools={}
    for path in sorted(root.glob('*/package.ini')):
        data=manifest(path)
        for name in data['tools'].split(','):
            if not name or name in tools:raise ValueError(f'duplicate/empty tool alias: {name}')
            tools[name]=(path.parent,data)
    return tools

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--package-root',type=Path,default=Path(__file__).resolve().parents[1]/'packages')
    p.add_argument('--wasmtime',default=os.environ.get('WASMTIME') or shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
    p.add_argument('--list-tools',action='store_true')
    p.add_argument('tool',nargs='?');p.add_argument('arguments',nargs=argparse.REMAINDER)
    a=p.parse_args();root=a.package_root.resolve()
    if not root.is_dir():p.error(f'package root missing: {root}')
    tools=discover(root)
    if a.list_tools:
        for name,(directory,data) in tools.items():print(f'{name}: {data["id"]} ({data["target"]}/{data["cpu"]}/{data["abi"]})')
        return 0
    if a.tool not in tools:p.error('specify one installed tool; use --list-tools')
    directory,data=tools[a.tool];module=directory/data['module']
    if not module.is_file():p.error(f'compiler module missing: {module}; run tools/build.py first')
    # Map project files and packages; the command owns no process-launch capability.
    cmd=[a.wasmtime,'run','--dir=.','--dir='+str(root)+'::/xh-packages',str(module),a.tool,'--package-root','/xh-packages',*a.arguments]
    return subprocess.run(cmd).returncode
if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError) as e:print(f'xhcc host: {e}',file=sys.stderr);sys.exit(1)
