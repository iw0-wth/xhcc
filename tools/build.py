"""Build the shared multi-target xhcc application and external configuration packs."""
from pathlib import Path
import argparse, os, shutil, subprocess, sys, json, hashlib
from build_cache import ObjectCache, compiler_identity, header_snapshot
ROOT=Path(__file__).resolve().parents[1]
TARGETS={'all':(0,'all'),'arm':(1,'arm'),'riscv32':(2,'riscv32'),'x86':(3,'x86'),'x86_64':(4,'x86_64'),'c':(5,'c')}
if os.name=='nt':
    pd=Path(sys.executable).parent
    os.environ['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),os.environ.get('PATH','')])
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--sdk',default=os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))
p.add_argument('--cc');p.add_argument('--native',action='store_true');p.add_argument('--verbose',action='store_true')
p.add_argument('--target',choices=TARGETS,default='all');p.add_argument('--all',action='store_true')
p.add_argument('--memory-mib',type=int,default=4)
p.add_argument('--arena-kib',type=int,default=1024)
p.add_argument('--profile',choices=['default','expanded'],default='default')
p.add_argument('--print-artifacts',action='store_true',help='print planned artifact paths as JSON without building')
p.add_argument('--app',choices=['xhcc','xhccm','xhccn','xhlinker','both','all-apps'],default='all-apps')
p.add_argument('--no-cache',action='store_true',help='compile every translation unit without object-cache reuse')
p.add_argument('--cache-dir',type=Path,default=ROOT/'build/object-cache')
p.add_argument('--wasm-validate',default=os.environ.get('WASM_VALIDATE') or shutil.which('wasm-validate') or (r'D:\wabt-1.0.42\bin\wasm-validate.exe' if os.name=='nt' else 'wasm-validate'))
p.add_argument('--wasm-opt',default=os.environ.get('WASM_OPT') or shutil.which('wasm-opt') or (r'D:\binaryen-version_133\bin\wasm-opt.exe' if os.name=='nt' else 'wasm-opt'))
a=p.parse_args()
if a.arena_kib<1 or a.arena_kib>1024*1024:p.error('--arena-kib must be 1..1048576')
if a.memory_mib<1 or a.memory_mib>4096:p.error('--memory-mib must be 1..4096')
if not a.native and a.arena_kib*1024>=a.memory_mib*1024*1024:p.error('arena must be smaller than declared linear memory; actual headroom still requires linking and runtime checks')
expanded=a.profile=='expanded' or a.arena_kib!=1024 or a.memory_mib!=4
profile='expanded' if expanded else 'default'
selected_targets=list(TARGETS) if a.all else [a.target]
selected_apps=['xhcc','xhccm','xhccn','xhlinker'] if a.app=='all-apps' else ['xhcc','xhccm'] if a.app=='both' else [a.app]
def artifact_dir(target,app):
    base=ROOT/'build'/('native' if a.native else 'wasm')/'dev'
    if expanded:return base/'expanded'/target/app
    base=base/target
    return base/app if app!='xhcc' else base
def artifact_path(target,app):
    return artifact_dir(target,app)/(app+'.exe' if a.native and os.name=='nt' else app if a.native else app+'.wasm')
if a.print_artifacts:
    print(json.dumps({'schema':'xhcc.build-plan.v1','profile':profile,'arena_bytes':a.arena_kib*1024,'memory_bytes':a.memory_mib*1024*1024,'publish_production':not expanded and not a.native,'artifacts':[{'target':t,'app':app,'path':str(artifact_path(t,app)),'metadata':str(artifact_dir(t,app)/'build-config.json')} for t in selected_targets for app in selected_apps]},indent=2));raise SystemExit(0)
cc=a.cc or str(Path(a.sdk)/'bin'/('clang.exe' if os.name=='nt' else 'clang'))
object_cache=ObjectCache(a.cache_dir,compiler_identity(cc),header_snapshot(ROOT,a.sdk),enabled=not a.no_cache)
def run(cmd):
    if a.verbose:print(' '.join(cmd),flush=True)
    r=subprocess.run(cmd)
    if r.returncode:raise SystemExit(r.returncode)
def build(target,app):
    num,package=TARGETS[target]
    out=artifact_dir(target,app)
    out.mkdir(parents=True,exist_ok=True)
    sources=[]
    for source in sorted((ROOT/'src').rglob('*.c')):
        rel=source.relative_to(ROOT/'src').as_posix()
        if app=='xhlinker':
            if rel not in ('link/main.c','link/linker_cli.c','link/elf_link.c','link/elf_read.c','link/arm_elf_reloc.c','link/rv_elf_reloc.c','link/ld_script.c','backend/rules/relocation.c','platform/wasi/flash_fs_host.c'):continue
        elif rel=='link/main.c':continue
        if app=='xhccn' and rel.startswith('link/'):continue
        if rel.startswith('stages/'):continue
        if rel.startswith('make/') and app!='xhccm':continue
        if rel.startswith('backend/riscv32/') and target not in ('all','riscv32'):continue
        if rel.startswith('backend/x86_64/') and target not in ('all','x86','x86_64'):continue
        if rel.startswith('backend/c/') and target not in ('all','c'):continue
        sources.append(source)
    if app!='xhlinker':sources+=sorted((ROOT/'object').glob('*.c'))
    flags=['-std=c99','-Oz','-g0','-ffunction-sections','-fdata-sections','-include',str(ROOT/'include/xh_small.h'),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'object'),f'-DXH_TARGET={num}',f'-DXH_MODULE_TARGET="{target}"','-Wno-unused-value','-Wno-incompatible-pointer-types']
    if a.arena_kib!=1024:flags+=[f'-DARK_POOL_SIZE={a.arena_kib*1024}']
    if not a.native:flags+=['--target=wasm32-wasip1','-mno-bulk-memory','-mno-sign-ext','-mno-nontrapping-fptoint','-mno-simd128','-mno-atomics']
    objects=[]
    for source in sources:
        obj=out/(source.relative_to(ROOT).as_posix().replace('/','_')+'.o')
        unit_flags=[*flags]
        if app=='xhccm' and source==ROOT/'src/driver.c':unit_flags+=['-DXH_DRIVER_NO_MAIN=1']
        if app=='xhccn' and source.relative_to(ROOT).as_posix() in ('src/driver.c','src/core/compiler.c','src/ir/ipo.c'):unit_flags+=['-DXH_WITH_LINKER=0']
        object_cache.compile(source,unit_flags,obj,run);objects.append(str(obj))
    output=artifact_path(target,app)
    link=[cc,*objects,'-o',str(output)]
    if not a.native:
        budget=a.memory_mib*1024*1024
        link+=['--target=wasm32-wasip1','-Wl,--gc-sections','-Wl,-z,stack-size=131072',f'-Wl,--initial-memory={budget}',f'-Wl,--max-memory={budget}','-Wl,-Map='+str(out/'xhcc.map')]
    run(link)
    if not a.native:
        lowered=out/'xhcc.lowered.wasm'
        run([a.wasm_opt,str(output),'-Oz','--signext-lowering','--llvm-nontrapping-fptoint-lowering','--llvm-memory-copy-fill-lowering','--disable-reference-types','--disable-multivalue','--disable-extended-const','--disable-bulk-memory','--disable-bulk-memory-opt','--disable-sign-ext','--disable-nontrapping-float-to-int','-o',str(lowered)])
        run([a.wasm_validate,'--disable-bulk-memory','--disable-reference-types','--disable-multi-value','--disable-sign-extension','--disable-saturating-float-to-int','--disable-simd','--disable-extended-const','--disable-tail-call','--disable-memory64','--disable-multi-memory',str(lowered)])
        shutil.copy2(output,out/'xhcc.sdk.wasm');os.replace(lowered,output)
        if target=='all' and not expanded:
            production=ROOT/'build/wasm'/(app+'.wasm')
            production.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(output,production)
    metadata={'schema':'xhcc.build-config.v1','profile':profile,'target':target,'app':app,'arena_bytes':a.arena_kib*1024,'memory_bytes':a.memory_mib*1024*1024,'stack_bytes':131072,'compiler':object_cache.identity,'compile_flags':flags,'link_command':link,'artifact':str(output),'artifact_sha256':hashlib.sha256(output.read_bytes()).hexdigest(),'artifact_bytes':output.stat().st_size,'map':str(out/'xhcc.map'),'published_production':target=='all' and not expanded and not a.native,'measurement_note':'Configured bounds only; runtime arena/heap peaks and full RSS are not inferred.'}
    (out/'build-config.json').write_text(json.dumps(metadata,indent=2)+'\n',encoding='utf-8')
    print(f'{app}/{target}: {output} ({output.stat().st_size} bytes)',flush=True)
for target in selected_targets:
    apps=selected_apps
    for app in apps:build(target,app)
print(f'Object cache: {object_cache.hits} hits, {object_cache.misses} compilations',flush=True)
