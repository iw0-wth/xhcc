"""RV32 external operation sequences: actual linking/execution and package mutation."""
from pathlib import Path
import os,sys,subprocess,tempfile,shutil,hashlib,unittest
if os.name=='nt':
 pd=Path(sys.executable).parent;os.environ['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),os.environ.get('PATH','')])
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'build/test-deps'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_RISCV,UC_MODE_RISCV32
from unicorn.riscv_const import UC_RISCV_REG_SP,UC_RISCV_REG_RA,UC_RISCV_REG_A0,UC_RISCV_REG_A1
SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'));OUT=ROOT/'build/rv-template-tests'
class RvTemplates(unittest.TestCase):
 @classmethod
 def setUpClass(cls):OUT.mkdir(parents=True,exist_ok=True)
 def compile(self,source,name='rv',package_root=None):
  path=OUT/(name+'.c');obj=OUT/(name+'.o');exe=OUT/(name+'.elf');path.write_text(source);cmd=[sys.executable,'tools/xhcc.py'];cmd+=['--package-root',str(package_root),'variant-gcc'] if package_root else ['riscv32-unknown-elf-gcc'];p=subprocess.run([*cmd,'-O0','-c',str(path.relative_to(ROOT)),'-o',str(obj.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);p=subprocess.run([str(SDK/'bin/ld.lld.exe'),'-m','elf32lriscv','--no-relax','-Ttext=0x10000','-e','entry','-o',str(exe),str(obj)],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);return exe
 def execute(self,exe,entry='entry',a=0,b=0):
  with exe.open('rb') as f:
   elf=ELFFile(f);sym=elf.get_section_by_name('.symtab').get_symbol_by_name(entry)[0]['st_value'];uc=Uc(UC_ARCH_RISCV,UC_MODE_RISCV32);uc.mem_map(0x10000,0x30000);uc.mem_map(0x700000,0x20000)
   for sec in elf.iter_sections():
    if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
   uc.reg_write(UC_RISCV_REG_SP,0x71f000);uc.reg_write(UC_RISCV_REG_RA,0x3f000);uc.reg_write(UC_RISCV_REG_A0,a&0xffffffff);uc.reg_write(UC_RISCV_REG_A1,b&0xffffffff);uc.emu_start(sym,0x3f000,count=100000);return uc.reg_read(UC_RISCV_REG_A0)&0xffffffff
 def test_unary_cast_memory_execution(self):
  source='''int entry(void){return 1;} int neg(int x){return -x;} int inv(int x){return ~x;} int zero(int x){return !x;} int s8(int x){return (signed char)x;} unsigned u8(unsigned x){return (unsigned char)x;} int s16(int x){return (short)x;} unsigned u16(unsigned x){return (unsigned short)x;} int memory(void){signed char a=-7;unsigned char b=250;short c=-300;unsigned short d=60000;return a+b+c+d;} int index(void){unsigned char a[3]={200,7,9}; a[1]=11; return a[0]+a[1]+a[2];} int farframe(void){char a[3000];a[2999]=23;return a[2999];}'''
  exe=self.compile(source)
  for name,values,expected in [('neg',[0,1,0xffffffff,0x80000000],lambda x:-x),('inv',[0,1,0xffffffff,0x80000000],lambda x:~x),('zero',[0,1,0xffffffff],lambda x:int(x==0)),('s8',[0,127,128,255,256,0xffffffff],lambda x:(x&255)-256 if x&128 else x&255),('u8',[0,255,256,0xffffffff],lambda x:x&255),('s16',[32767,32768,65535,65536,0xffffffff],lambda x:(x&65535)-65536 if x&32768 else x&65535),('u16',[65535,65536,0xffffffff],lambda x:x&65535)]:
   for value in values:self.assertEqual(self.execute(exe,name,value),expected(value)&0xffffffff,(name,value))
  self.assertEqual(self.execute(exe,'memory'),59943);self.assertEqual(self.execute(exe,'index'),220);self.assertEqual(self.execute(exe,'farframe'),23)
 def test_abi_long_arguments_varargs_and_safe_frame_variant(self):
  source='''#include <stdarg.h>
int ten(int a,int b,int c,int d,int e,int f,int g,int h,int i,int j){return a+b+c+d+e+f+g+h+i+j;} long long wide(long long a,int b,long long c,int d,int e,int f,int g){return a+b+c+d+e+f+g;} int sum(int count,...){va_list ap;int i,total=0;va_start(ap,count);for(i=0;i<count;i++)total+=va_arg(ap,int);va_end(ap);return total;} struct T{int a,b,c;}; struct T make(int x){struct T t={x,x+1,x+2};return t;} int entry(void){int(*fp)(int,int,int,int,int,int,int,int,int,int)=ten;struct T t=make(10);return fp(1,2,3,4,5,6,7,8,9,10)+sum(10,1,2,3,4,5,6,7,8,9,10)+(int)wide(4294967296LL,2,5LL,3,4,5,6)+t.a+t.b+t.c;}'''
  compiler=ROOT/'build/wasm/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest()
  with tempfile.TemporaryDirectory(prefix='xh-rv-abi-') as tmp:
   root=Path(tmp);pkg=root/'variant';shutil.copytree(ROOT/'packages/riscv32',pkg);manifest=(pkg/'package.ini').read_text();(pkg/'package.ini').write_text('\n'.join('id=variant' if x.startswith('id=') else 'tools=variant-gcc' if x.startswith('tools=') else x for x in manifest.splitlines())+'\n');rules=pkg/'rv.rules';original=rules.read_text()
   baseline=self.compile(source,'abi0',root);self.assertEqual(self.execute(baseline),168)
   rules.write_text(original.replace('rule frame.extra_padding\ntext 0\n','rule frame.extra_padding\ntext 32\n'));padded=self.compile(source,'abi32',root);self.assertEqual(self.execute(padded),168);self.assertNotEqual(baseline.read_bytes(),padded.read_bytes())
   for old,new in [('rule abi.arg_registers\ntext 8\n','rule abi.arg_registers\ntext 4\n'),('rule frame.extra_padding\ntext 0\n','rule frame.extra_padding\ntext 7\n'),('rule classify.wide_words\ntext 2\n','rule classify.wide_words\ntext 1\n')]:
    rules.write_text(original.replace(old,new));path=OUT/'invalidabi.c';path.write_text('int entry(void){return 1;}');p=subprocess.run([sys.executable,'tools/xhcc.py','--package-root',str(root),'variant-gcc','-c',str(path.relative_to(ROOT)),'-o',str((OUT/'invalidabi.o').relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertNotEqual(p.returncode,0);self.assertIn('external policy',p.stderr)
  self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)
 def test_external_semantic_selection_and_effect_validation(self):
  compiler=ROOT/'build/wasm/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest()
  with tempfile.TemporaryDirectory(prefix='xh-rv-semantic-') as tmp:
   root=Path(tmp);pkg=root/'variant';shutil.copytree(ROOT/'packages/riscv32',pkg);manifest=(pkg/'package.ini').read_text();(pkg/'package.ini').write_text('\n'.join('id=variant' if x.startswith('id=') else 'tools=variant-gcc' if x.startswith('tools=') else x for x in manifest.splitlines())+'\n');rules=pkg/'rv.rules';original=rules.read_text();source='int entry(int x){return -x;}'
   baseline=self.compile(source,'semantic0',root);self.assertEqual(self.execute(baseline,a=7),0xfffffff9)
   rules.write_text(original.replace('unary neg 32 v stack unary.neg.i32 1 1','unary neg 32 v stack unary.not.i32 1 1'));changed=self.compile(source,'semantic1',root);self.assertEqual(self.execute(changed,a=7),0xfffffff8)
   source='int entry(int x){return (signed char)x;}';rules.write_text(original.replace('cast trunc 8 s stack cast.trunc.s8 1 1','cast trunc 8 s stack cast.trunc.u8 1 1'));changed=self.compile(source,'semantic_cast',root);self.assertEqual(self.execute(changed,a=255),255)
   for bad in [original.replace('unary neg 32 v stack unary.neg.i32 1 1','unary neg 32 v stack unary.neg.i32 0 1'),original.replace('unary neg 32 v stack unary.neg.i32 1 1','unary neg 32 v stack unary.neg.i32 1 1\\nunary neg 32 v stack unary.not.i32 1 1')]:
    rules.write_text(bad);path=OUT/'semantic_bad.c';path.write_text('int entry(int x){return -x;}');p=subprocess.run([sys.executable,'tools/xhcc.py','--package-root',str(root),'variant-gcc','-c',str(path.relative_to(ROOT)),'-o',str((OUT/'semantic_bad.o').relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertNotEqual(p.returncode,0)
  self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)
 def test_external_unary_and_cast_variants(self):
  compiler=ROOT/'build/wasm/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest()
  with tempfile.TemporaryDirectory(prefix='xh-rv-template-') as tmp:
   root=Path(tmp);pkg=root/'variant';shutil.copytree(ROOT/'packages/riscv32',pkg);manifest=(pkg/'package.ini').read_text();(pkg/'package.ini').write_text('\n'.join('id=variant' if x.startswith('id=') else 'tools=variant-gcc' if x.startswith('tools=') else x for x in manifest.splitlines())+'\n');rules=pkg/'rv.rules';original=rules.read_text();source='int entry(int x){return -x;}';exe=self.compile(source,'before',root);self.assertEqual(self.execute(exe,a=7),0xfffffff9)
   rules.write_text(original.replace('    neg t0, t0\\n','    not t0, t0\\n',1));exe=self.compile(source,'after',root);self.assertEqual(self.execute(exe,a=7),0xfffffff8)
   rules.write_text(original.replace('rule unary.neg.i32','rule removed.unary.neg.i32',1));path=OUT/'bad.c';path.write_text(source);p=subprocess.run([sys.executable,'tools/xhcc.py','--package-root',str(root),'variant-gcc','-c',str(path.relative_to(ROOT)),'-o',str((OUT/'bad.o').relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertNotEqual(p.returncode,0)
  self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)
if __name__=='__main__':unittest.main()
