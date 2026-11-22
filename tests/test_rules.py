"""Build/run the actual C encoding VM in Wasm; validate fields and external changes.
The full compiler integration is also exercised by test_targets.py (RV32 ELF
linking and Unicorn execution). This test needs WASI SDK and Wasmtime only.
"""
from pathlib import Path
import os, subprocess, sys, unittest
ROOT=Path(__file__).resolve().parents[1]
if os.name=='nt':
 pd=Path(sys.executable).parent;os.environ['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),os.environ.get('PATH','')])
SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))
WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
OUT=ROOT/'build/rules-tests'
HARNESS=r'''
#include "xh_rules.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
int fs_file_size(const char*p){FILE*f=fopen(p,"rb");long n;if(!f)return -1;fseek(f,0,SEEK_END);n=ftell(f);fclose(f);return(int)n;}
int fs_read_file(const char*p,char*b,int cap){FILE*f=fopen(p,"rb");int n;if(!f)return -1;n=(int)fread(b,1,cap-1,f);b[n]=0;fclose(f);return n;}
void diag_tool(int sev,const char*who,const char*msg,const char*a0,const char*a1,const char*a2,const char*a3,const char*a4,const char*a5){(void)sev;fprintf(stderr,"%s: %s\n",who,msg);}
int main(int argc,char**argv){unsigned w;int a[8],i;if(argc<3)return 2;if(argc==4&&!strcmp(argv[2],"select")){char out[96];int r;if(xh_rules_set_selection_path(argv[1]))return 3;r=xh_rules_select_binary(argv[3],out,sizeof(out));if(r<0)return 1;if(r>0)fputs(out,stdout);else puts("FALLBACK");return 0;}if(xh_rules_set_path(argv[1]))return 3;for(i=3;i<argc;i++)a[i-3]=(int)strtol(argv[i],0,0);if(xh_rules_encode(argv[2],a,argc-3,&w))return 1;printf("%08x\n",w);xh_rules_release();return 0;}
'''
class EncodingRules(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  OUT.mkdir(exist_ok=True,parents=True);(OUT/'harness.c').write_text(HARNESS)
  env=os.environ.copy();env['PATH']=str(Path(sys.executable).parent)+os.pathsep+str(Path(sys.executable).parent/'Library/bin')+os.pathsep+env.get('PATH','')
  subprocess.run([str(SDK/'bin/clang.exe'),'--target=wasm32-wasip1','-O2','-Iinclude','-Iconfig','-include','include/xh_small.h','build/rules-tests/harness.c','src/backend/rules/encoder.c','src/backend/rules/selector.c','-o','build/rules-tests/vm.wasm'],cwd=ROOT,env=env,check=True,capture_output=True)
 def encode(self,rule,args,path='packages/riscv32/encoding.rules',ok=True):
  p=subprocess.run([WASM,'run','--dir=.','build/rules-tests/vm.wasm',path,rule,*map(str,args)],cwd=ROOT,capture_output=True,text=True)
  if ok:self.assertEqual(p.returncode,0,p.stderr);return int(p.stdout.strip(),16)
  self.assertNotEqual(p.returncode,0);return p.stderr
 def test_selector_vm_and_fallback(self):
  p=subprocess.run([WASM,'run','--dir=.','build/rules-tests/vm.wasm','packages/riscv32/selection.rules','select','ADD'],cwd=ROOT,capture_output=True,text=True)
  self.assertEqual(p.returncode,0,p.stderr);self.assertEqual(p.stdout,'    add t0, t0, t1\n')
  p=subprocess.run([WASM,'run','--dir=.','build/rules-tests/vm.wasm','packages/riscv32/selection.rules','select','COMPARE'],cwd=ROOT,capture_output=True,text=True)
  self.assertEqual(p.returncode,0,p.stderr);self.assertEqual(p.stdout,'FALLBACK\n')
 def test_standard_formats(self):
  self.assertEqual(self.encode('R',[0,12,11,0,10,0x33]),0x00c58533)
  self.assertEqual(self.encode('I',[-1,11,0,10,0x13]),0xfff58513)
  self.assertEqual(self.encode('S',[-4,10,2,2,0x23]),0xfea12e23)
  self.assertEqual(self.encode('B',[16,0,10,0,0x63]),0x00050863)
  self.assertEqual(self.encode('U',[0x12345,10,0x37]),0x12345537)
  self.assertEqual(self.encode('J',[8,1,0x6f]),0x008000ef)
 def test_range_and_alignment(self):
  self.assertIn('range',self.encode('I',[2048,1,0,2,0x13],ok=False))
  self.assertIn('alignment',self.encode('B',[3,0,1,0,0x63],ok=False))
  self.assertIn('range',self.encode('R',[0,32,1,0,2,0x33],ok=False))
 def test_external_variant_without_rebuild(self):
  p=OUT/'variant.rules';base=(ROOT/'packages/riscv32/encoding.rules').read_text();p.write_text(base.replace('bits 1 0 5 20','bits 1 0 5 15',1).replace('bits 2 0 5 15','bits 2 0 5 20',1))
  standard=self.encode('R',[0,12,11,0,10,0x33]);variant=self.encode('R',[0,12,11,0,10,0x33],str(p.relative_to(ROOT)))
  self.assertNotEqual(standard,variant)
 def test_variant_package_execution_without_compiler_rebuild(self):
  import shutil, tempfile, struct, hashlib
  sys.path.insert(0,str(ROOT/'build/test-deps'))
  from elftools.elf.elffile import ELFFile
  from unicorn import Uc,UC_ARCH_RISCV,UC_MODE_RISCV32
  from unicorn.riscv_const import UC_RISCV_REG_SP,UC_RISCV_REG_RA,UC_RISCV_REG_A0
  compiler=ROOT/'build/wasm/xhcc.wasm'
  if not compiler.is_file():self.skipTest('build compiler first')
  original_hash=hashlib.sha256(compiler.read_bytes()).digest()
  source=OUT/'variant.c';source.write_text('int entry(void){return 4096;}\n')
  base=(ROOT/'packages/riscv32/encoding.rules').read_text()
  with tempfile.TemporaryDirectory(prefix='xh-rules-') as tmp:
   root=Path(tmp);pkg=root/'variant';pkg.mkdir();(pkg/'include').mkdir()
   manifest=(ROOT/'packages/riscv32/package.ini').read_text().replace('id=riscv32','id=variant').replace('tools=riscv32-unknown-elf-gcc,riscv32-none-elf-gcc','tools=variant-gcc')
   (pkg/'package.ini').write_text(manifest)
   shutil.copy2(ROOT/'packages/riscv32/selection.rules',pkg/'selection.rules')
   outputs=[]
   for variant in [False,True]:
    rules=base.replace('bits 0 0 20 12','bits 0 0 19 13',1) if variant else base
    (pkg/'encoding.rules').write_text(rules)
    obj=OUT/('variant.o' if variant else 'standard.o');exe=obj.with_suffix('.elf')
    p=subprocess.run([sys.executable,'tools/xhcc.py','--package-root',str(root),'variant-gcc','-c',str(source.relative_to(ROOT)),'-o',str(obj.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True)
    self.assertEqual(p.returncode,0,p.stderr)
    p=subprocess.run([str(SDK/'bin/ld.lld.exe'),'-m','elf32lriscv','--no-relax','-Ttext=0x10000','-e','entry','-o',str(exe),str(obj)],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
    with exe.open('rb') as f:
     elf=ELFFile(f);entry=elf.get_section_by_name('.symtab').get_symbol_by_name('entry')[0]['st_value'];uc=Uc(UC_ARCH_RISCV,UC_MODE_RISCV32);uc.mem_map(0x10000,0x20000);uc.mem_map(0x700000,0x20000)
     for sec in elf.iter_sections():
      if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
     uc.reg_write(UC_RISCV_REG_SP,0x71f000);uc.reg_write(UC_RISCV_REG_RA,0x2f000);uc.emu_start(entry,0x2f000,count=100000);outputs.append(uc.reg_read(UC_RISCV_REG_A0)&0xffffffff)
   self.assertEqual(outputs,[4096,8192])
   (pkg/'encoding.rules').write_text('XHENC 999\n')
   rejected=OUT/'rejected.o'
   if rejected.exists():rejected.unlink()
   p=subprocess.run([sys.executable,'tools/xhcc.py','--package-root',str(root),'variant-gcc','-c',str(source.relative_to(ROOT)),'-o',str(rejected.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True)
   self.assertNotEqual(p.returncode,0);self.assertIn('invalid encoding rules',p.stderr);self.assertFalse(rejected.exists())
  self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),original_hash)
 def test_selection_package_execution_without_compiler_rebuild(self):
  import shutil, tempfile, struct, hashlib
  sys.path.insert(0,str(ROOT/'build/test-deps'))
  from elftools.elf.elffile import ELFFile
  from unicorn import Uc,UC_ARCH_RISCV,UC_MODE_RISCV32
  from unicorn.riscv_const import UC_RISCV_REG_SP,UC_RISCV_REG_RA,UC_RISCV_REG_A0
  compiler=ROOT/'build/wasm/xhcc.wasm'
  if not compiler.is_file():self.skipTest('build compiler first')
  original_hash=hashlib.sha256(compiler.read_bytes()).digest()
  source=OUT/'selection.c';source.write_text('int entry(void){int a=10,b=3;return a+b;}\n')
  base=(ROOT/'packages/riscv32/selection.rules').read_text()
  with tempfile.TemporaryDirectory(prefix='xh-rules-') as tmp:
   root=Path(tmp);pkg=root/'variant';pkg.mkdir();(pkg/'include').mkdir()
   manifest=(ROOT/'packages/riscv32/package.ini').read_text().replace('id=riscv32','id=variant').replace('tools=riscv32-unknown-elf-gcc,riscv32-none-elf-gcc','tools=variant-gcc')
   (pkg/'package.ini').write_text(manifest)
   shutil.copy2(ROOT/'packages/riscv32/encoding.rules',pkg/'encoding.rules')
   outputs=[]
   for variant in [False,True]:
    rules=base.replace('select ADD scalar32 add','select ADD scalar32 sub',1) if variant else base
    (pkg/'selection.rules').write_text(rules)
    obj=OUT/('variant.o' if variant else 'standard.o');exe=obj.with_suffix('.elf')
    p=subprocess.run([sys.executable,'tools/xhcc.py','--package-root',str(root),'variant-gcc','-c',str(source.relative_to(ROOT)),'-o',str(obj.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True)
    self.assertEqual(p.returncode,0,p.stderr)
    p=subprocess.run([str(SDK/'bin/ld.lld.exe'),'-m','elf32lriscv','--no-relax','-Ttext=0x10000','-e','entry','-o',str(exe),str(obj)],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
    with exe.open('rb') as f:
     elf=ELFFile(f);entry=elf.get_section_by_name('.symtab').get_symbol_by_name('entry')[0]['st_value'];uc=Uc(UC_ARCH_RISCV,UC_MODE_RISCV32);uc.mem_map(0x10000,0x20000);uc.mem_map(0x700000,0x20000)
     for sec in elf.iter_sections():
      if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
     uc.reg_write(UC_RISCV_REG_SP,0x71f000);uc.reg_write(UC_RISCV_REG_RA,0x2f000);uc.emu_start(entry,0x2f000,count=100000);outputs.append(uc.reg_read(UC_RISCV_REG_A0)&0xffffffff)
   self.assertEqual(outputs,[13,7])
   (pkg/'selection.rules').write_text('XHSELECT 999\n')
   rejected=OUT/'rejected.o'
   if rejected.exists():rejected.unlink()
   p=subprocess.run([sys.executable,'tools/xhcc.py','--package-root',str(root),'variant-gcc','-c',str(source.relative_to(ROOT)),'-o',str(rejected.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True)
   self.assertNotEqual(p.returncode,0);self.assertIn('invalid selection rules',p.stderr);self.assertFalse(rejected.exists())
  self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),original_hash)
 def test_unsigned_numeric_overflow(self):
  p=OUT/'overflow.rules';p.write_text('XHENC 1\nrule X 1 4294967296\narg 0 5 0 1\nbits 0 0 5 0\nend\n')
  self.assertIn('invalid encoding rules',self.encode('X',[1],str(p.relative_to(ROOT)),ok=False))
 def test_reject_bad_rules(self):
  p=OUT/'bad.rules';p.write_text('XHENC 1\nrule X 1 0\narg 0 5 0 1\nbits 0 0 5 0\nbits 0 0 1 0\nend\n')
  self.assertIn('invalid encoding rules',self.encode('X',[1],str(p.relative_to(ROOT)),ok=False))
if __name__=='__main__':unittest.main()
