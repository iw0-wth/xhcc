"""Represent significant trailing spaces explicitly in template rules."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
for path in ROOT.glob('packages/*/*.rules'):
    lines=path.read_text(encoding='utf-8').splitlines()
    normalized=[]
    for line in lines:
        if line.startswith('text') and line.endswith(' '):
            body=line[5:] if line.startswith('text ') else ''
            n=len(body)-len(body.rstrip(' '))
            line='text '+body.rstrip(' ')+r'\x20'*n if body else 'text'
        normalized.append(line)
    path.write_text('\n'.join(normalized)+'\n',encoding='utf-8')
