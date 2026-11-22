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
int main(int argc,char**argv){unsigned w;int a[8],i;if(argc<3)return 2;if(argc>=4&&!strcmp(argv[2],"armencode")){if(xh_encoding_set_path("arm",argv[1]))return 1;for(i=4;i<argc;i++)a[i-4]=(int)strtol(argv[i],0,0);if(xh_encoding_encode("arm",argv[3],a,argc-4,&w))return 1;printf("%08x\n",w);return 0;}if(argc==5&&!strcmp(argv[2],"switch")){int z[]={3};unsigned rv,first,second;if(xh_encoding_set_path("arm",argv[1])||xh_encoding_set_path("variant",argv[3])||xh_encoding_encode("arm","T",z,1,&first)||xh_encoding_encode("variant","T",z,1,&second)||xh_rules_encode("I",(int[]){1,0,0,10,19},5,&rv)||xh_encoding_encode("arm","T",z,1,&w))return 1;printf("%x %x %x %x\n",first,second,w,rv);return 0;}if(argc==4&&!strcmp(argv[2],"select")){char out[96];int r;if(xh_rules_set_selection_path(argv[1]))return 3;r=xh_rules_select_binary(argv[3],out,sizeof(out));if(r<0)return 1;if(r>0)fputs(out,stdout);else puts("FALLBACK");return 0;}if(xh_rules_set_path(argv[1]))return 3;for(i=3;i<argc;i++)a[i-3]=(int)strtol(argv[i],0,0);if(xh_rules_encode(argv[2],a,argc-3,&w))return 1;printf("%08x\n",w);xh_rules_release();return 0;}
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
 def test_constant_encoding_rule(self):
  path=OUT/'constant.encoding';path.write_text('XHENC 1\nrule NOP 0 0xbf00\nend\n')
  p=subprocess.run([WASM,'run','--dir=.','build/rules-tests/vm.wasm',str(path.relative_to(ROOT)),'armencode','NOP'],cwd=ROOT,capture_output=True,text=True)
  self.assertEqual(p.returncode,0,p.stderr);self.assertEqual(int(p.stdout.strip(),16),0xbf00)
 def test_arm_rule_bytes_against_gnu(self):
  import struct
  arm=Path(os.environ.get('ARM_BIN',r'D:\arm-none-eabi\bin'))
  if not (arm/'arm-none-eabi-as.exe').exists():self.skipTest('GNU ARM assembler not available')
  cases=[('MOV_IMM',[3,219],'movs r3,#219',16),('MOV_REG',[9,12],'mov r9,r12',16),('ADD_IMM3',[1,2,7],'adds r1,r2,#7',16),('LSL_IMM',[2,5,17],'lsls r2,r5,#17',16),('MOVW',[9,0xabcd],'movw r9,#0xabcd',32),('MOVT',[11,0x8765],'movt r11,#0x8765',32),('ADDW',[9,10,4095],'addw r9,r10,#4095',32),('SUBW',[8,7,2049],'subw r8,r7,#2049',32),('SDIV',[9,10,11],'sdiv r9,r10,r11',32),('UDIV',[2,3,4],'udiv r2,r3,r4',32)]
  expected=b'';source='.syntax unified\n.thumb\n.cpu cortex-m4\n.text\n'
  for name,args,assembly,width in cases:
   p=subprocess.run([WASM,'run','--dir=.','build/rules-tests/vm.wasm','packages/arm/arm.encoding.rules','armencode',name,*map(str,args)],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);word=int(p.stdout.strip(),16);expected+=struct.pack('<H',word) if width==16 else struct.pack('<HH',word>>16,word&65535);source+=assembly+'\n'
  (OUT/'gnu.s').write_text(source);subprocess.run([str(arm/'arm-none-eabi-as.exe'),str(OUT/'gnu.s'),'-o',str(OUT/'gnu.o')],check=True,capture_output=True);subprocess.run([str(arm/'arm-none-eabi-objcopy.exe'),'-O','binary','-j','.text',str(OUT/'gnu.o'),str(OUT/'gnu.bin')],check=True,capture_output=True);self.assertEqual((OUT/'gnu.bin').read_bytes(),expected)
 def test_target_specific_encoding_cache(self):
  arm=OUT/'arm.encoding';variant=OUT/'variant.encoding'
  arm.write_text('XHENC 1\nrule T 1 0x2000\narg 0 3 0 1\nbits 0 0 3 0\nend\n')
  variant.write_text('XHENC 1\nrule T 1 0x30000000\narg 0 3 0 1\nbits 0 0 3 16\nend\n')
  p=subprocess.run([WASM,'run','--dir=.','build/rules-tests/vm.wasm',str(arm.relative_to(ROOT)),'switch',str(variant.relative_to(ROOT)),'ignored'],cwd=ROOT,capture_output=True,text=True)
  self.assertEqual(p.returncode,0,p.stderr);self.assertEqual(p.stdout.strip(),'2003 30030000 2003 100513')
 def test_selector_vm_and_fallback(self):
  p=subprocess.run([WASM,'run','--dir=.','build/rules-tests/vm.wasm','packages/riscv32/selection.rules','select','ADD'],cwd=ROOT,capture_output=True,text=True)
  self.assertEqual(p.returncode,0,p.stderr);self.assertEqual(p.stdout,'    add t0, t0, t1\n')
  p=subprocess.run([WASM,'run','--dir=.','build/rules-tests/vm.wasm','packages/riscv32/selection.rules','select','COMPARE'],cwd=ROOT,capture_output=True,text=True)
  self.assertEqual(p.returncode,0,p.stderr);self.assertEqual(p.stdout,'FALLBACK\n')
 def test_selector_v1_compatibility_and_sequence_rejection(self):
  path=OUT/'compat.selection';path.write_text('XHSELECT 1\nselect ADD scalar32 add dst lhs rhs\n')
  def invoke():return subprocess.run([WASM,'run','--dir=.','build/rules-tests/vm.wasm',str(path.relative_to(ROOT)),'select','ADD'],cwd=ROOT,capture_output=True,text=True)
  p=invoke();self.assertEqual(p.returncode,0,p.stderr);self.assertEqual(p.stdout,'    add t0, t0, t1\n')
  invalid=['XHSELECT 2\nselect ADD scalar32\nend\n','XHSELECT 2\nselect ADD scalar32\nemit add dst lhs rhs\n','XHSELECT 2\nselect ADD scalar32\n'+'emit add dst lhs rhs\n'*9+'end\n','XHSELECT 2\nselect ADD scalar32\nemit xori dst dst rhs\nend\n','XHSELECT 2\nselect ADD scalar32\nemit seqz rhs lhs\nend\n']
  for text in invalid:
   path.write_text(text);p=invoke();self.assertNotEqual(p.returncode,0,p.stdout);self.assertIn('invalid selection rules',p.stderr)
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
   root=Path(tmp);pkg=root/'variant';shutil.copytree(ROOT/'packages/riscv32',pkg)
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
   root=Path(tmp);pkg=root/'variant';shutil.copytree(ROOT/'packages/riscv32',pkg)
   manifest=(ROOT/'packages/riscv32/package.ini').read_text().replace('id=riscv32','id=variant').replace('tools=riscv32-unknown-elf-gcc,riscv32-none-elf-gcc','tools=variant-gcc')
   (pkg/'package.ini').write_text(manifest)
   shutil.copy2(ROOT/'packages/riscv32/encoding.rules',pkg/'encoding.rules')
   outputs=[]
   for variant in [False,True]:
    rules=base.replace('emit add dst lhs rhs','emit sub dst lhs rhs',1) if variant else base
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
 def test_scalar32_sequence_boundaries(self):
  sys.path.insert(0,str(ROOT/'build/test-deps'))
  from elftools.elf.elffile import ELFFile
  from unicorn import Uc,UC_ARCH_RISCV,UC_MODE_RISCV32
  from unicorn.riscv_const import UC_RISCV_REG_SP,UC_RISCV_REG_RA,UC_RISCV_REG_A0,UC_RISCV_REG_A1
  operations={'add':'+','sub':'-','mul':'*','band':'&','bor':'|','bxor':'^','shl':'<<','shr':'>>','eq':'==','ne':'!=','lt':'<','gt':'>','le':'<=','ge':'>=','div':'/','mod':'%'}
  text=''.join(f'{typ} {prefix}_{name}({typ} a,{typ} b){{return a {op} b;}}\n' for prefix,typ in [('s','int'),('u','unsigned')] for name,op in operations.items())
  source=OUT/'boundaries.c';source.write_text(text);obj=OUT/'boundaries.o';exe=OUT/'boundaries.elf'
  p=subprocess.run([sys.executable,'tools/xhcc.py','riscv32-unknown-elf-gcc','-O0','-c',str(source.relative_to(ROOT)),'-o',str(obj.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
  subprocess.run([str(SDK/'bin/ld.lld.exe'),'-m','elf32lriscv','--no-relax','-Ttext=0x10000','-e','s_add','-o',str(exe),str(obj)],cwd=ROOT,check=True,capture_output=True)
  with exe.open('rb') as f:
   elf=ELFFile(f);symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()};uc=Uc(UC_ARCH_RISCV,UC_MODE_RISCV32);uc.mem_map(0x10000,0x30000);uc.mem_map(0x700000,0x20000)
   for sec in elf.iter_sections():
    if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
   count=0
   for unsigned in [False,True]:
    for name,op in operations.items():
     pairs=[(0,1),(1,1),(0xffffffff,1),(0x80000000,0x7fffffff),(0x7fffffff,0xffffffff),(0xfffffff9,3),(17,5)]
     if name in ['shl','shr']:pairs=[(a,b) for a in [0,1,0xffffffff,0x80000000,0x7fffffff] for b in [0,1,15,31]]
     for a,b in pairs:
      aa=a if unsigned or a<0x80000000 else a-0x100000000;bb=b if unsigned or b<0x80000000 else b-0x100000000
      if name in ['div','mod']:
       if bb==0:continue
       q=abs(aa)//abs(bb);q=-q if (aa<0)!=(bb<0) else q;want=q if name=='div' else aa-q*bb
      elif name=='band':want=aa&bb
      elif name=='bor':want=aa|bb
      elif name=='bxor':want=aa^bb
      else:want=eval(f'aa {op} bb')
      uc.reg_write(UC_RISCV_REG_SP,0x71f000);uc.reg_write(UC_RISCV_REG_RA,0x3f000);uc.reg_write(UC_RISCV_REG_A0,a);uc.reg_write(UC_RISCV_REG_A1,b);uc.emu_start(symbols[('u' if unsigned else 's')+'_'+name],0x3f000,count=100000);got=uc.reg_read(UC_RISCV_REG_A0)&0xffffffff
      self.assertEqual(got,int(want)&0xffffffff,(unsigned,name,hex(a),hex(b),hex(got),want));count+=1
  # Empty v2 table explicitly exercises the preserved C fallback; default
  # sequences must produce identical machine text for all 32 functions.
  import tempfile, shutil
  with tempfile.TemporaryDirectory(prefix='xh-fallback-') as tmp:
   root=Path(tmp);pkg=root/'fallback';shutil.copytree(ROOT/'packages/riscv32',pkg)
   manifest=(ROOT/'packages/riscv32/package.ini').read_text().replace('id=riscv32','id=fallback').replace('tools=riscv32-unknown-elf-gcc,riscv32-none-elf-gcc','tools=fallback-gcc');(pkg/'package.ini').write_text(manifest)
   shutil.copy2(ROOT/'packages/riscv32/encoding.rules',pkg/'encoding.rules');(pkg/'selection.rules').write_text('XHSELECT 2\n')
   fallback=OUT/'fallback.o';p=subprocess.run([sys.executable,'tools/xhcc.py','--package-root',str(root),'fallback-gcc','-O0','-c',str(source.relative_to(ROOT)),'-o',str(fallback.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
   with obj.open('rb') as a,fallback.open('rb') as b:self.assertEqual(ELFFile(a).get_section_by_name('.text').data(),ELFFile(b).get_section_by_name('.text').data())
  self.assertEqual(count,276)
  print(f'PASS RV32 scalar32 sequence execution: {count} signed/unsigned arithmetic, bitwise, shift, compare, DIV/MOD cases')
 def test_unsigned_numeric_overflow(self):
  p=OUT/'overflow.rules';p.write_text('XHENC 1\nrule X 1 4294967296\narg 0 5 0 1\nbits 0 0 5 0\nend\n')
  self.assertIn('invalid encoding rules',self.encode('X',[1],str(p.relative_to(ROOT)),ok=False))
 def test_reject_bad_rules(self):
  p=OUT/'bad.rules';p.write_text('XHENC 1\nrule X 1 0\narg 0 5 0 1\nbits 0 0 5 0\nbits 0 0 1 0\nend\n')
  self.assertIn('invalid encoding rules',self.encode('X',[1],str(p.relative_to(ROOT)),ok=False))
if __name__=='__main__':unittest.main()
