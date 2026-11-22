"""WASI/package/capacity regressions; stdlib only."""
from pathlib import Path
import importlib.util, shutil, subprocess, tempfile, unittest, sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from wasm_audit import audit
RUNTIME=shutil.which('wasmtime') or r'C:\Program Files\Wasmtime\bin\wasmtime.exe'
class PlatformTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='platform-',dir=ROOT/'build');self.dir=Path(self.temp.name);self.rel=self.dir.relative_to(ROOT).as_posix()
    def tearDown(self):self.temp.cleanup()
    def run_cc(self,*args,ok=True):
        cmd=[RUNTIME,'run','--dir=.',str(ROOT/'build/wasm/xhcc.wasm'),*args]
        r=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True,timeout=30)
        self.assertEqual(r.returncode==0,ok,r.stdout+r.stderr);return r
    def test_memory_and_wasi_contract(self):
        result=audit(ROOT/'build/wasm/xhcc.wasm');self.assertEqual(result['memories'][0]['maximum_bytes'],3145728)
    def test_nested_includes_and_dependency_escaping(self):
        d=self.dir/'space dir';d.mkdir();(d/'nested').mkdir();(d/'nested/b.h').write_text('#define VALUE 19\n');(d/'a.h').write_text('#include "nested/b.h"\n');(d/'main.c').write_text('#include "a.h"\nint answer(void){return VALUE;}\n')
        source=(d/'main.c').relative_to(ROOT).as_posix();dep=self.rel+'/result.d';out=self.rel+'/result.o'
        self.run_cc('-c','-MMD','-MF',dep,'-MT','obj with space','-O0','-o',out,source)
        text=(ROOT/dep).read_text();self.assertIn('obj\\ with\\ space:',text);self.assertIn('space\\ dir/nested/b.h',text)
    def test_errors_and_limits(self):
        source=self.dir/'large.c';source.write_text(' '*(32*1024));out=self.rel+'/bad.o'
        self.run_cc('-c','-o',out,self.rel+'/large.c',ok=False);self.assertFalse((ROOT/out).exists())
        source.write_text('int f( { return; }\n');self.run_cc('-c','-o',out,self.rel+'/large.c',ok=False);self.assertFalse((ROOT/out).exists())
        for i in range(10):(self.dir/f'h{i}.h').write_text(f'#include "h{i+1}.h"\n' if i<9 else '#define END 1\n')
        source.write_text('#include "h0.h"\nint f(void){return END;}\n');self.run_cc('-c','-o',out,self.rel+'/large.c',ok=False)
    def test_arena_exhaustion_and_io_failure(self):
        source=self.dir/'arena.c';source.write_text('int f(void){int x=0;\n'+('x=x+1;\n'*3000)+'return x;}\n')
        r=self.run_cc('-c',self.rel+'/arena.c','-o',self.rel+'/arena.o',ok=False)
        self.assertIn('memory pool exhausted',r.stderr)
        self.assertFalse((self.dir/'arena.o').exists())
        source.write_text('int f(void){return 1;}\n')
        self.run_cc('-c',self.rel+'/arena.c','-o',self.rel+'/absent/out.o',ok=False)
    def test_copy_resources_and_alias_conflicts(self):
        root=self.dir/'packages';root.mkdir();shutil.copytree(ROOT/'packages/arm-cortex-m3',root/'arm-cortex-m3',ignore=shutil.ignore_patterns('compiler.wasm'))
        manifest=root/'arm-cortex-m3/package.ini';text=manifest.read_text();manifest.write_text(text.replace('arm-none-eabi-gcc,armcc','arm-none-eabi-gcc,armcc,newcc'))
        self.assertIn('newcc',self.run_cc('--package-root',root.relative_to(ROOT).as_posix(),'--list-tools').stdout)
        (self.dir/'alias.c').write_text('int alias(void){return 3;}\n')
        self.run_cc('newcc','--package-root',root.relative_to(ROOT).as_posix(),'-c',self.rel+'/alias.c','-o',self.rel+'/alias.o')
        shutil.copytree(root/'arm-cortex-m3',root/'duplicate');p=root/'duplicate/package.ini';p.write_text(text.replace('id=arm-cortex-m3','id=duplicate'))
        self.run_cc('--package-root',root.relative_to(ROOT).as_posix(),'--list-tools',ok=False)
if __name__=='__main__':unittest.main()
