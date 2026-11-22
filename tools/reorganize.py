"""One-time source adoption migration (kept for provenance)."""
from pathlib import Path
import shutil
ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'vendor/arkcc'
GROUPS={
 'frontend/c': ['agginit','ast','lexer','parser','preproc','sema','symtab','type'],
 'ir': ['ir','irgen','irser','mir','mirpass','opt','opt_loop','opt_tailmerge','pipeline','ipo'],
 'diagnostics': ['diag','diag_msg_table','enumcheck','memlint','unusedresult'],
 'core': ['compiler','config','cpu','memory','scatter','codegen_common'],
}
def main():
    if not BASE.is_dir():raise SystemExit('already migrated, or source tree missing')
    for group,names in GROUPS.items():
        dest=ROOT/'src'/group;dest.mkdir(parents=True,exist_ok=True)
        for name in names:
            source=BASE/'src'/(name+'.c');target=dest/source.name
            if target.exists():raise RuntimeError(f'target exists: {target}')
            source.rename(target)
    for source in (BASE/'include').glob('*.h'):
        target=ROOT/'include'/source.name
        if target.exists():raise RuntimeError(f'target exists: {target}')
        source.rename(target)
    for rel,dest in [('src/backend/arm','src/backend/arm'),('src/backend/registry.c','src/backend/registry.c'),('port/host','src/platform/wasi')]:
        target=ROOT/dest;target.parent.mkdir(parents=True,exist_ok=True);(BASE/rel).rename(target)
    shutil.copy2(BASE/'LICENSE',ROOT/'LICENSE')
    # Verify the exact obsolete import subtree before removing unused backends,
    # STM32 shell and old main (all recoverable from the reference Git commit).
    expected=ROOT/'vendor'
    if expected.resolve()!=Path(__file__).resolve().parents[1]/'vendor':raise RuntimeError('unexpected removal target')
    shutil.rmtree(expected)
if __name__=='__main__':main()
