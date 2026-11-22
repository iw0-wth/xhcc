"""Inspect Wasm memory/import contract without third-party dependencies."""
from pathlib import Path
import argparse, json

def uleb(b, p):
    n=shift=0
    while True:
        v=b[p];p+=1;n|=(v&127)<<shift
        if not v&128:return n,p
        shift+=7
        if shift>63:raise ValueError('oversized LEB')
def string(b,p):
    n,p=uleb(b,p);return b[p:p+n].decode(),p+n

def audit(path):
    b=Path(path).read_bytes()
    if b[:8]!=b'\0asm\x01\0\0\0':raise ValueError('not a Wasm v1 module')
    p=8;imports=[];memories=[];features=[]
    while p<len(b):
        sid=b[p];size,p=uleb(b,p+1);end=p+size
        if end>len(b):raise ValueError('truncated section')
        if sid==2:
            count,q=uleb(b,p)
            for _ in range(count):
                mod,q=string(b,q);name,q=string(b,q);kind=b[q];q+=1
                if kind!=0:raise ValueError('non-function import is outside command profile')
                _,q=uleb(b,q);imports.append([mod,name])
        elif sid==5:
            count,q=uleb(b,p)
            for _ in range(count):
                flags,q=uleb(b,q);minimum,q=uleb(b,q)
                if flags!=1:raise ValueError('memory requires explicit unshared wasm32 maximum')
                maximum,q=uleb(b,q);memories.append({'initial_pages':minimum,'maximum_pages':maximum,'maximum_bytes':maximum*65536})
        elif sid==0:
            name,q=string(b,p)
            if name=='target_features':
                count,q=uleb(b,q)
                for _ in range(count):
                    prefix=chr(b[q]);q+=1;feature,q=string(b,q);features.append(prefix+feature)
        p=end
    if len(memories)!=1 or memories[0]['maximum_bytes']>3*1024*1024:raise ValueError('module exceeds 3 MiB memory contract')
    if any(m!='wasi_snapshot_preview1' for m,n in imports):raise ValueError('unexpected host import')
    forbidden={'simd128','relaxed-simd','atomics','memory64','exception-handling','multimemory','tail-call'}
    if any(f.startswith('+') and f[1:] in forbidden for f in features):raise ValueError('unsupported feature')
    return {'file_bytes':len(b),'memories':memories,'imports':imports,'target_features':features,'note':'Import compatibility and runtime overhead require WAMR/NuttX configuration validation; feature metadata is not an instruction-by-instruction proof.'}
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('module',nargs='?',default=str(Path(__file__).resolve().parents[1]/'build/wasm/xhcc.wasm'));a=p.parse_args();print(json.dumps(audit(a.module),indent=2))
