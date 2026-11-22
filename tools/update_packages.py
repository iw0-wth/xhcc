"""Write configuration-only built-in package manifests; no Wasm code per package."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
for name in ['arm-cortex-m3','riscv32','x86','x86_64','c']:
 p=ROOT/'packages'/name/'package.ini'
 old=dict(line.split('=',1) for line in p.read_text().splitlines() if line and not line.startswith('#'))
 keys=['id','tools','target','cpu','abi','include']
 fields={'version':'3',**{key:old[key] for key in keys}}
 if name!='arm-cortex-m3':fields['dialect']='gcc'
 p.write_text(''.join(f'{k}={v}\n' for k,v in fields.items()),encoding='utf-8')
