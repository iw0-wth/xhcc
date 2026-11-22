"""Actual package-driven x86 code generation, LLVM link and Unicorn execution.
No compiler build is performed: run after the lead's unified --all build.
Aggregate tests exercise xhcc's existing private wire protocol, including its
>8-byte hidden-pointer convention. They do not establish GCC/Clang aggregate
SysV ABI interoperability. Tool aliases currently implement a target subset.
"""
from pathlib import Path
import hashlib, os, shutil, struct, subprocess, sys, tempfile, unittest
ROOT=Path(__file__).resolve().parents[1]
if os.name=='nt':
 pd=Path(sys.executable).parent
 os.environ['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),os.environ.get('PATH','')])
sys.path.insert(0,str(ROOT/'build/test-deps'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_MODE_64
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_RSP, UC_X86_REG_EAX, UC_X86_REG_RAX
SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))/'bin'
OUT=ROOT/'build/x86-rule-tests'

class X86Rules(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  OUT.mkdir(parents=True,exist_ok=True)
  cls.compiler=ROOT/'build/wasm/xhcc.wasm'
  if not cls.compiler.is_file():raise unittest.SkipTest('unified compiler build required')
 def invoke(self,root,tool,source,obj,ok=True):
  p=subprocess.run([sys.executable,'tools/xhcc.py','--package-root',str(root),tool,'-O0','-c',str(source.relative_to(ROOT)),'-o',str(obj.relative_to(ROOT))],cwd=ROOT,text=True,capture_output=True)
  if ok:self.assertEqual(p.returncode,0,p.stdout+p.stderr)
  else:self.assertNotEqual(p.returncode,0);self.assertFalse(obj.exists())
  return p
 def execute(self,obj,wide):
  exe=obj.with_suffix('.elf')
  p=subprocess.run([str(SDK/'ld.lld.exe'),'-m','elf_x86_64' if wide else 'elf_i386','--image-base=0','-Ttext=0x10000','-e','entry','-o',str(exe),str(obj)],cwd=ROOT,text=True,capture_output=True)
  self.assertEqual(p.returncode,0,p.stderr)
  with exe.open('rb') as f:
   elf=ELFFile(f);entry=elf.get_section_by_name('.symtab').get_symbol_by_name('entry')[0]['st_value']
   uc=Uc(UC_ARCH_X86,UC_MODE_64 if wide else UC_MODE_32);uc.mem_map(0x10000,0x30000);uc.mem_map(0x700000,0x20000)
   for sec in elf.iter_sections():
    if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
   sp=0x71eff8 if wide else 0x71effc
   uc.reg_write(UC_X86_REG_RSP if wide else UC_X86_REG_ESP,sp);uc.mem_write(sp,struct.pack('<Q' if wide else '<I',0x3f000));uc.emu_start(entry,0x3f000,count=200000)
   return uc.reg_read(UC_X86_REG_RAX if wide else UC_X86_REG_EAX)&0xffffffff
 def package(self,root,target):
  pkg=root/'variant';shutil.copytree(ROOT/'packages'/target,pkg)
  manifest=(pkg/'package.ini').read_text();manifest+='\n'
  manifest='\n'.join('id=variant' if line.startswith('id=') else 'tools=variant-gcc' if line.startswith('tools=') else line for line in manifest.splitlines())
  if 'templates=' not in manifest:manifest+='\ntemplates=x86.rules\n'
  (pkg/'package.ini').write_text(manifest)
  return pkg
 def test_rules_change_execution_same_compiler_sha(self):
  before=hashlib.sha256(self.compiler.read_bytes()).digest()
  for target in ['x86','x86_64']:
   with self.subTest(target=target),tempfile.TemporaryDirectory(prefix='xh-x86-') as tmp:
    root=Path(tmp);pkg=self.package(root,target);rules=pkg/'x86.rules';base=rules.read_text()
    source=OUT/'variant.c';source.write_text('int entry(void){int a=10,b=3;return a+b;}\n')
    outputs=[]
    for changed in [False,True]:
     start=base.index('rule scalar32.s.add\n');end=base.index('\nend',start)
     variant=base[:start]+base[start:end].replace('add eax, ecx','sub eax, ecx')+base[end:] if changed else base
     rules.write_text(variant);obj=OUT/(target+('_changed' if changed else '_base')+'.o')
     self.invoke(root,'variant-gcc',source,obj);outputs.append(self.execute(obj,target=='x86_64'))
    self.assertEqual(outputs,[13,7])
    # The encoder selection table is externally authoritative too.
    self.assertIn('rule encoding.xa_alu.1.3.5.0.0\ntext 1\n',base)
    rules.write_text(base.replace('rule encoding.xa_alu.1.3.5.0.0\ntext 1\n','rule encoding.xa_alu.1.3.5.0.0\ntext 41\n'))
    obj=OUT/(target+'_encoding.o');self.invoke(root,'variant-gcc',source,obj);self.assertEqual(self.execute(obj,target=='x86_64'),7)
    # Missing and malformed required rules reject output.
    for idx,bad in enumerate(['XHTEMPLATE 999\n',base.replace('rule scalar32.s.add\n','rule missing.scalar32.s.add\n'),base.replace('rule abi.integer.count\ntext 6\n','rule abi.integer.count\ntext 5\n') if target=='x86_64' else base.replace('rule abi.integer.count\ntext 0\n','rule abi.integer.count\ntext 1\n')]):
     rules.write_text(bad);obj=OUT/(target+f'_rejected{idx}.o')
     if obj.exists():obj.unlink()
     self.invoke(root,'variant-gcc',source,obj,ok=False)
  self.assertEqual(hashlib.sha256(self.compiler.read_bytes()).digest(),before)
 def test_rex_width_rule_changes_machine_execution(self):
  source=OUT/'rex.c';source.write_text('int entry(void){int result; asm volatile("mov %0, 1\\nshl rax, 32\\nshr rax, 31" : "=r"(result)); return result;}\n')
  before=hashlib.sha256(self.compiler.read_bytes()).digest()
  with tempfile.TemporaryDirectory(prefix='xh-x86-') as tmp:
   root=Path(tmp);pkg=self.package(root,'x86_64');path=pkg/'x86.rules';base=path.read_text();outputs=[]
   self.assertIn('rule encoding.rex.w\ntext 8\n',base)
   for changed in [False,True]:
    path.write_text(base.replace('rule encoding.rex.w\ntext 8\n','rule encoding.rex.w\ntext 0\n') if changed else base)
    obj=OUT/('rex_changed.o' if changed else 'rex_base.o');self.invoke(root,'variant-gcc',source,obj);outputs.append(self.execute(obj,True))
   self.assertEqual(outputs,[2,0])
  self.assertEqual(hashlib.sha256(self.compiler.read_bytes()).digest(),before)
 def test_rep_table_drives_aggregate_copy(self):
  source=OUT/'rep.c';source.write_text('struct S{int a,b,c,d,e;}; int entry(void){struct S a={1,2,3,4,5},b; b=a; return b.a+b.e;}\n')
  before=hashlib.sha256(self.compiler.read_bytes()).digest()
  for target in ['x86','x86_64']:
   with self.subTest(target=target),tempfile.TemporaryDirectory(prefix='xh-x86-') as tmp:
    root=Path(tmp);pkg=self.package(root,target);path=pkg/'x86.rules';base=path.read_text();outputs=[]
    self.assertIn('rule encoding.rep.0.op\ntext 164\n',base)
    for changed in [False,True]:
     path.write_text(base.replace('rule encoding.rep.0.op\ntext 164\n','rule encoding.rep.0.op\ntext 170\n') if changed else base)
     obj=OUT/(target+('_rep_changed.o' if changed else '_rep_base.o'));self.invoke(root,'variant-gcc',source,obj);outputs.append(self.execute(obj,target=='x86_64'))
    self.assertEqual(outputs[0],6);self.assertNotEqual(outputs[1],6)
  self.assertEqual(hashlib.sha256(self.compiler.read_bytes()).digest(),before)
 def test_abi_layout_sret_packed_varargs_and_fp_mix(self):
  source=OUT/'abi_layout.c';source.write_text('''#include <stdarg.h>
struct Big{int a,b,c,d,e;};
struct __attribute__((packed)) Packed{char a; int b; short c;};
struct Big make(int n){struct Big r={n,n+1,n+2,n+3,n+4};return r;}
int consume(struct Big b){return b.a+b.e;}
int packed(struct Packed p){return p.a+p.b+p.c;}
double mix(int a,double b,int c,double d,int e,double f,int g,double h,int i,double j,int k,double l,int m,double n,int o,double p,double q){return a+b+c+d+e+f+g+h+i+j+k+l+m+n+o+p+q;}
int vari(int count,...){va_list ap;int sum=0,i;va_start(ap,count);for(i=0;i<count;i++)sum+=va_arg(ap,int);va_end(ap);return sum;}
int varied(int count,...){va_list ap;double sum=0;int i;va_start(ap,count);for(i=0;i<count;i++)sum+=va_arg(ap,double);va_end(ap);return (int)sum;}
int entry(void){struct Big a=make(7),b=make(11);struct Big (*fn)(int)=make;struct Big c=fn(20);struct Packed p={2,30,4};
return consume(a)==18 && consume(b)==26 && consume(c)==44 && packed(p)==36 && sizeof(p)==7
&& vari(9,1,2,3,4,5,6,7,8,9)==45 && varied(10,1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0,10.0)==55
&& (int)mix(1,2.0,3,4.0,5,6.0,7,8.0,9,10.0,11,12.0,13,14.0,15,16.0,17.0)==153;}
''')
  for target in ['x86','x86_64']:
   with self.subTest(target=target),tempfile.TemporaryDirectory(prefix='xh-x86-') as tmp:
    root=Path(tmp);pkg=self.package(root,target);obj=OUT/(target+'_abi_layout.o');self.invoke(root,'variant-gcc',source,obj);self.assertEqual(self.execute(obj,target=='x86_64'),1)
    rules=pkg/'x86.rules';base=rules.read_text()
    for idx,(key,value) in enumerate([('abi.frame.alignment','8'),('abi.va.fp_stride','8'),('abi.aggregate.direct_limit','16'),('abi.aggregate.class.1.wire','4'),('abi.frame.scratch_bytes','256'),('abi.frame.multiword_scratch_base','0')]):
     import re
     changed=re.sub('(rule '+re.escape(key)+'\\ntext )[^\\n]*',lambda m:m[1]+value,base)
     self.assertNotEqual(base,changed);rules.write_text(changed);obj=OUT/(target+f'_abi_reject{idx}.o')
     if obj.exists():obj.unlink()
     self.invoke(root,'variant-gcc',source,obj,ok=False)
    rules.write_text(base.replace('rule abi.va.save_bytes\n','rule missing.abi.va.save_bytes\n'))
    obj=OUT/(target+'_abi_missing.o')
    if obj.exists():obj.unlink()
    self.invoke(root,'variant-gcc',source,obj,ok=False)
 def test_scalar_boundaries(self):
  # Division/shift unsignedness, narrow types and floating sequences cross the
  # actual compiler/assembler/linker boundary on both x86 modes.
  source=OUT/'boundaries.c';source.write_text('''unsigned udiv(unsigned a,unsigned b){return a/b;}
unsigned ushr(unsigned a,unsigned b){return a>>b;}
int entry(void){unsigned a=0x80000000u; signed char c=-3; unsigned short s=65530; double d=7.5; int x=-17,y=5; unsigned z=0xfffffff9u;
return udiv(a,2)==0x40000000u && ushr(a,31)==1 && c==-3 && s==65530 && (int)(d+2.5)==10
&& x+y==-12 && x-y==-22 && x*y==-85 && x/y==-3 && x%y==-2
&& (x>>2)==-5 && (y<<3)==40 && (x&y)==5 && (x|y)==-17 && (x^y)==-22
&& x<y && y>x && x<=y && y>=x && x!=y && !(x==y)
&& z/3==1431655763u && z%3==0 && z>y && z>=y && !(z<y) && !(z<=y);}
''')
  for target in ['x86','x86_64']:
   with self.subTest(target=target),tempfile.TemporaryDirectory(prefix='xh-x86-') as tmp:
    root=Path(tmp);self.package(root,target);obj=OUT/(target+'_boundaries.o');self.invoke(root,'variant-gcc',source,obj);self.assertEqual(self.execute(obj,target=='x86_64'),1)
if __name__=='__main__':unittest.main()
