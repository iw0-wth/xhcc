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
 def test_softfloat_helper_selection_and_conversion_abi(self):
  # Integer helper fixtures return observable IEEE bit patterns. They verify
  # soft-call ABI and record selection, not a general IEEE math implementation.
  source='''union F{float f;unsigned u;}; union D{double d;unsigned long long u;};
int __eqsf2(unsigned a,unsigned b){return 0;} int __nesf2(unsigned a,unsigned b){return 1;} int __ltsf2(unsigned a,unsigned b){return -1;} int __gtsf2(unsigned a,unsigned b){return 1;} int __lesf2(unsigned a,unsigned b){return 0;} int __gesf2(unsigned a,unsigned b){return 0;} int __eqdf2(long long a,long long b){return 0;} int __nedf2(long long a,long long b){return 1;} int __ltdf2(long long a,long long b){return -1;} int __gtdf2(long long a,long long b){return 1;} int __ledf2(long long a,long long b){return 0;} int __gedf2(long long a,long long b){return 0;}
unsigned __addsf3(unsigned a,unsigned b){return 0x40800000u;} unsigned __subsf3(unsigned a,unsigned b){return 0x3f800000u;} unsigned __mulsf3(unsigned a,unsigned b){return 0x41000000u;} unsigned __divsf3(unsigned a,unsigned b){return 0x3f000000u;} long long __adddf3(long long a,long long b){return 0x4010000000000000LL;} long long __subdf3(long long a,long long b){return 0x3ff0000000000000LL;} long long __muldf3(long long a,long long b){return 0x4020000000000000LL;} long long __divdf3(long long a,long long b){return 0x3fe0000000000000LL;}
int __fixsfsi(unsigned a){return 17;} unsigned __fixunssfsi(unsigned a){return 19;} int __fixdfsi(long long a){return 23;} unsigned __fixunsdfsi(long long a){return 29;} unsigned __floatsisf(int a){return 0x40000000u;} unsigned __floatunsisf(unsigned a){return 0x40400000u;} long long __floatsidf(int a){return 0x4000000000000000LL;} long long __floatunsidf(unsigned a){return 0x4008000000000000LL;}
unsigned __floatdisf(long long a){return 0x40a00000u;} unsigned __floatundisf(unsigned long long a){return 0x40c00000u;} long long __floatdidf(long long a){return 0x4014000000000000LL;} long long __floatundidf(unsigned long long a){return 0x4018000000000000LL;} long long __fixsfdi(unsigned a){return 31;} unsigned long long __fixunssfdi(unsigned a){return 37;} long long __fixdfdi(long long a){return 41;} unsigned long long __fixunsdfdi(long long a){return 43;}
long long __extendsfdf2(unsigned a){return 0x4010000000000000LL;} unsigned __truncdfsf2(long long a){return 0x40800000u;}
unsigned l2f(void){union F a;long long x=5;a.f=(float)x;return a.u;} unsigned ul2f(void){union F a;unsigned long long x=6;a.f=(float)x;return a.u;} unsigned l2d(void){union D a;long long x=5;a.d=(double)x;return (unsigned)(a.u>>32);} unsigned ul2d(void){union D a;unsigned long long x=6;a.d=(double)x;return (unsigned)(a.u>>32);} unsigned f2l(void){union F a;a.u=1;return (unsigned)(long long)a.f;} unsigned f2ul(void){union F a;a.u=1;return (unsigned)(unsigned long long)a.f;} unsigned d2l(void){union D a;a.u=1;return (unsigned)(long long)a.d;} unsigned d2ul(void){union D a;a.u=1;return (unsigned)(unsigned long long)a.d;}
unsigned entry(void){union F a,b,c;a.u=0x3fc00000u;b.u=0x40200000u;c.f=a.f+b.f;return c.u;} unsigned mul(void){union F a,b,c;a.u=1;b.u=2;c.f=a.f*b.f;return c.u;} unsigned div(void){union F a,b,c;a.u=1;b.u=2;c.f=a.f/b.f;return c.u;} unsigned dadd(void){union D a,b,c;a.u=1;b.u=2;c.d=a.d+b.d;return (unsigned)(c.u>>32);} int f2i(void){union F a;a.u=0x40800000u;return (int)a.f;} unsigned f2u(void){union F a;a.u=0x40800000u;return (unsigned)a.f;} int d2i(void){union D a;a.u=1;return (int)a.d;} unsigned d2u(void){union D a;a.u=1;return (unsigned)a.d;} unsigned i2f(void){union F a;int x=2;a.f=(float)x;return a.u;} unsigned u2f(void){union F a;unsigned x=3;a.f=(float)x;return a.u;} unsigned i2d(void){union D a;int x=2;a.d=(double)x;return (unsigned)(a.u>>32);} unsigned u2d(void){union D a;unsigned x=3;a.d=(double)x;return (unsigned)(a.u>>32);} unsigned narrow(void){union D a;union F b;a.u=1;b.f=(float)a.d;return b.u;} unsigned widen(void){union F a;union D b;a.u=1;b.d=(double)a.f;return (unsigned)(b.u>>32);}'''
  for typename,prefix in [('float','fc'),('double','dc')]:
   for name,op in [('eq','=='),('ne','!='),('lt','<'),('gt','>'),('le','<='),('ge','>=')]:source+=f' int {prefix}_{name}({typename} a,{typename} b){{return a {op} b;}}'
  exe=self.compile(source,'softfixture')
  for prefix in ['fc','dc']:
   for name in ['eq','ne','lt','gt','le','ge']:self.assertEqual(self.execute(exe,prefix+'_'+name),1,prefix+'_'+name)
  for name,want in [('entry',0x40800000),('mul',0x41000000),('div',0x3f000000),('dadd',0x40100000),('f2i',17),('f2u',19),('d2i',23),('d2u',29),('i2f',0x40000000),('u2f',0x40400000),('i2d',0x40000000),('u2d',0x40080000),('narrow',0x40800000),('widen',0x40100000),('l2f',0x40a00000),('ul2f',0x40c00000),('l2d',0x40140000),('ul2d',0x40180000),('f2l',31),('f2ul',37),('d2l',41),('d2ul',43)]:self.assertEqual(self.execute(exe,name),want,name)
  compiler=ROOT/'build/wasm/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest()
  with tempfile.TemporaryDirectory(prefix='xh-rv-float-') as tmp:
   root=Path(tmp);pkg=root/'variant';shutil.copytree(ROOT/'packages/riscv32',pkg);manifest=(pkg/'package.ini').read_text();(pkg/'package.ini').write_text('\n'.join('id=variant' if x.startswith('id=') else 'tools=variant-gcc' if x.startswith('tools=') else x for x in manifest.splitlines())+'\n');rules=pkg/'rv.rules';rules.write_text(rules.read_text().replace('float.bin add 32 real stack softfloat.add.f32 2 1','float.bin add 32 real stack softfloat.sub.f32 2 1'));changed=self.compile(source,'softvariant',root);self.assertEqual(self.execute(changed),0x3f800000)
  self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)
 def test_integer64_sequences_boundaries_and_selection_variant(self):
  operations={'add':'+','sub':'-','band':'&','bor':'|','bxor':'^','mul':'*','shl':'<<','shr':'>>','div':'/','mod':'%','eq':'==','ne':'!=','lt':'<','gt':'>','le':'<=','ge':'>='}
  source='int entry(void){return 0;}'+''.join(f'{typ} {prefix}_{name}({typ} a,{typ} b){{return a {op} b;}}' for prefix,typ in [('s','long long'),('u','unsigned long long')] for name,op in operations.items())
  exe=self.compile(source,'integer64');mask=(1<<64)-1
  with exe.open('rb') as f:
   elf=ELFFile(f);symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()};uc=Uc(UC_ARCH_RISCV,UC_MODE_RISCV32);uc.mem_map(0x10000,0x30000);uc.mem_map(0x700000,0x20000)
   from unicorn.riscv_const import UC_RISCV_REG_A2,UC_RISCV_REG_A3
   for sec in elf.iter_sections():
    if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
   count=0
   for unsigned in [False,True]:
    for name,op in operations.items():
     pairs=[(0,1),(1,1),(mask,3),(1<<63,7),((1<<63)-1,0xffffffff),(0x1234567887654321,0x102030405)]
     if name in ['shl','shr']:pairs=[(a,b) for a in [1,mask,1<<63,0x1234567887654321] for b in [0,1,31,32,33,63]]
     for a,b in pairs:
      aa=a if unsigned or a<(1<<63) else a-(1<<64);bb=b if unsigned or b<(1<<63) else b-(1<<64)
      if name in ['div','mod']:
       q=abs(aa)//abs(bb);q=-q if (aa<0)!=(bb<0) else q;want=q if name=='div' else aa-q*bb
      elif name=='band':want=aa&bb
      elif name=='bor':want=aa|bb
      elif name=='bxor':want=aa^bb
      else:want=eval(f'aa {op} bb')
      uc.reg_write(UC_RISCV_REG_SP,0x71f000);uc.reg_write(UC_RISCV_REG_RA,0x3f000)
      for register,value in [(UC_RISCV_REG_A0,a&0xffffffff),(UC_RISCV_REG_A1,a>>32),(UC_RISCV_REG_A2,b&0xffffffff),(UC_RISCV_REG_A3,b>>32)]:uc.reg_write(register,value)
      uc.emu_start(symbols[('u' if unsigned else 's')+'_'+name],0x3f000,count=100000);got=(uc.reg_read(UC_RISCV_REG_A0)&0xffffffff)|((uc.reg_read(UC_RISCV_REG_A1)&0xffffffff)<<32);self.assertEqual(got,int(want)&mask,(unsigned,name,hex(a),hex(b),hex(got),hex(int(want)&mask)));count+=1
  self.assertEqual(count,264)
  compiler=ROOT/'build/wasm/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest()
  with tempfile.TemporaryDirectory(prefix='xh-rv-i64-') as tmp:
   root=Path(tmp);pkg=root/'variant';shutil.copytree(ROOT/'packages/riscv32',pkg);manifest=(pkg/'package.ini').read_text();(pkg/'package.ini').write_text('\n'.join('id=variant' if x.startswith('id=') else 'tools=variant-gcc' if x.startswith('tools=') else x for x in manifest.splitlines())+'\n');rules=pkg/'rv.rules';text=rules.read_text();simple='unsigned entry(void){long long a=10,b=3;return (unsigned)(a+b);}';normal=self.compile(simple,'i64normal',root);self.assertEqual(self.execute(normal),13);rules.write_text(text.replace('integer64 add 64 s stack integer64.add.s 4 2','integer64 add 64 s stack integer64.sub.s 4 2'));variant=self.compile(simple,'i64variant',root);self.assertEqual(self.execute(variant),7)
  self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)
 def test_complex_sequence_selection_abi_only(self):
  # Stubs intentionally return known bit patterns and validate marshalling;
  # they are not IEEE/libm implementations.
  source='''union Z{double _Complex z; unsigned long long words[2];}; double _Complex conj(double _Complex);
long long __adddf3(long long a,long long b){return 0x4010000000000000LL;} long long __subdf3(long long a,long long b){return 0x3ff0000000000000LL;} long long __muldf3(long long a,long long b){return 0x4020000000000000LL;} long long __divdf3(long long a,long long b){return 0x3fe0000000000000LL;} int __eqdf2(long long a,long long b){return 0;} long long __floatsidf(int a){return 0x4000000000000000LL;} long long __floatunsidf(unsigned a){return 0x4008000000000000LL;} long long __extendsfdf2(unsigned a){return 0x4010000000000000LL;}
unsigned entry(void){union Z a,b,c;a.words[0]=1;a.words[1]=2;b.words[0]=3;b.words[1]=4;c.z=a.z+b.z;return (unsigned)(c.words[0]>>32);} unsigned sub(void){union Z a,b,c;a.words[0]=1;a.words[1]=2;b.words[0]=3;b.words[1]=4;c.z=a.z-b.z;return (unsigned)(c.words[1]>>32);} unsigned mul(void){union Z a,b,c;a.words[0]=1;a.words[1]=2;b.words[0]=3;b.words[1]=4;c.z=a.z*b.z;return (unsigned)(c.words[0]>>32);} unsigned div(void){union Z a,b,c;a.words[0]=1;a.words[1]=2;b.words[0]=3;b.words[1]=4;c.z=a.z/b.z;return (unsigned)(c.words[0]>>32);} int eq(void){union Z a,b;a.words[0]=1;a.words[1]=2;b.words[0]=3;b.words[1]=4;return a.z==b.z;} int ne(void){union Z a,b;a.words[0]=1;a.words[1]=2;b.words[0]=3;b.words[1]=4;return a.z!=b.z;} unsigned conjbits(void){union Z a,b;a.words[0]=1;a.words[1]=0x4000000000000000LL;b.z=conj(a.z);return (unsigned)(b.words[1]>>32);} unsigned tildebits(void){union Z a,b;a.words[0]=1;a.words[1]=0x4000000000000000LL;b.z=~a.z;return (unsigned)(b.words[1]>>32);} unsigned fromint(void){union Z a;int x=2;a.z=(double _Complex)x;return (unsigned)(a.words[0]>>32);}'''
  exe=self.compile(source,'complexfixture')
  for name,want in [('entry',0x40100000),('sub',0x3ff00000),('mul',0x3ff00000),('div',0x3fe00000),('eq',1),('ne',0),('conjbits',0xc0000000),('tildebits',0xc0000000),('fromint',0x40000000)]:self.assertEqual(self.execute(exe,name),want,name)
  compiler=ROOT/'build/wasm/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest()
  with tempfile.TemporaryDirectory(prefix='xh-rv-complex-') as tmp:
   root=Path(tmp);pkg=root/'variant';shutil.copytree(ROOT/'packages/riscv32',pkg);manifest=(pkg/'package.ini').read_text();(pkg/'package.ini').write_text('\n'.join('id=variant' if x.startswith('id=') else 'tools=variant-gcc' if x.startswith('tools=') else x for x in manifest.splitlines())+'\n');rules=pkg/'rv.rules';text=rules.read_text();rules.write_text(text.replace('complex.bin add 128 complex stack complex.add 8 4','complex.bin add 128 complex stack complex.sub 8 4'));changed=self.compile(source,'complexvariant',root);self.assertEqual(self.execute(changed),0x3ff00000)
  self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)
 def test_control_call_return_semantic_records(self):
  compiler=ROOT/'build/wasm/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest()
  source='int one(int x){return x+7;} int entry(int x){if(x)return one(4);return one(8);} int loops(void){int i,total=0;for(i=0;i<5;i++){if(i==2)continue;total+=i;}return total;}'
  exe=self.compile(source,'protocol');self.assertEqual(self.execute(exe,a=0),15);self.assertEqual(self.execute(exe,a=1),11);self.assertEqual(self.execute(exe,'loops'),8)
  with tempfile.TemporaryDirectory(prefix='xh-rv-protocol-') as tmp:
   root=Path(tmp);pkg=root/'variant';shutil.copytree(ROOT/'packages/riscv32',pkg);manifest=(pkg/'package.ini').read_text();(pkg/'package.ini').write_text('\n'.join('id=variant' if x.startswith('id=') else 'tools=variant-gcc' if x.startswith('tools=') else x for x in manifest.splitlines())+'\n');rules=pkg/'rv.rules';original=rules.read_text()
   rules.write_text(original.replace('control zero 32 v local control.zero 1 0','control zero 32 v local control.nonzero 1 0'));changed=self.compile(source,'protocol_changed',root);self.assertEqual(self.execute(changed,a=0),11);self.assertEqual(self.execute(changed,a=1),15)
   for old,new in [('argument word 32 v register argument.word.register 0 0','argument word 32 v register argument.word.register 1 0'),('return value 32 v scalar return.value.scalar 1 0','return value 32 v scalar return.value.scalar 0 0')]:
    rules.write_text(original.replace(old,new));path=OUT/'protocol_bad.c';path.write_text(source);p=subprocess.run([sys.executable,'tools/xhcc.py','--package-root',str(root),'variant-gcc','-c',str(path.relative_to(ROOT)),'-o',str((OUT/'protocol_bad.o').relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertNotEqual(p.returncode,0)
  self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)
 def test_indexed_wide_complex_and_packed_sequences(self):
  source='''struct __attribute__((packed)) P{char a;unsigned long long x;}; union Z{double _Complex z;unsigned long long w[2];}; unsigned entry(void){unsigned long long a[3]={1,0x1234567887654321ULL,3};a[2]=a[1]+5;return (unsigned)(a[2]>>32)^(unsigned)a[2];} unsigned packed64(void){struct P p;p.a=1;p.x=0x1234567887654321ULL;return (unsigned)(p.x>>32)^(unsigned)p.x;} unsigned complex_index(void){union Z a,b;a.w[0]=0x1122334455667788ULL;a.w[1]=0x99aabbccddeeff00ULL;b.z=*(double _Complex*)&a;return (unsigned)(b.w[0]>>32)^(unsigned)b.w[1];}'''
  exe=self.compile(source,'indexedwide');self.assertEqual(self.execute(exe),0x12345678^0x87654326);self.assertEqual(self.execute(exe,'packed64'),0x12345678^0x87654321);self.assertEqual(self.execute(exe,'complex_index'),0x11223344^0xddeeff00)
  compiler=ROOT/'build/wasm/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest()
  with tempfile.TemporaryDirectory(prefix='xh-rv-indexed-') as tmp:
   root=Path(tmp);pkg=root/'variant';shutil.copytree(ROOT/'packages/riscv32',pkg);manifest=(pkg/'package.ini').read_text();(pkg/'package.ini').write_text('\n'.join('id=variant' if x.startswith('id=') else 'tools=variant-gcc' if x.startswith('tools=') else x for x in manifest.splitlines())+'\n');rules=pkg/'rv.rules';text=rules.read_text()
   bad=text.replace('memory store 64 v index_aligned semantic.index.store.wide.aligned 3 2','memory store 64 v index_aligned semantic.index.store.wide.aligned 2 2');rules.write_text(bad);path=OUT/'indexed_bad.c';path.write_text(source);p=subprocess.run([sys.executable,'tools/xhcc.py','--package-root',str(root),'variant-gcc','-c',str(path.relative_to(ROOT)),'-o',str((OUT/'indexed_bad.o').relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertNotEqual(p.returncode,0)
  self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)
 def test_external_unary_and_cast_variants(self):
  compiler=ROOT/'build/wasm/xhcc.wasm';digest=hashlib.sha256(compiler.read_bytes()).digest()
  with tempfile.TemporaryDirectory(prefix='xh-rv-template-') as tmp:
   root=Path(tmp);pkg=root/'variant';shutil.copytree(ROOT/'packages/riscv32',pkg);manifest=(pkg/'package.ini').read_text();(pkg/'package.ini').write_text('\n'.join('id=variant' if x.startswith('id=') else 'tools=variant-gcc' if x.startswith('tools=') else x for x in manifest.splitlines())+'\n');rules=pkg/'rv.rules';original=rules.read_text();source='int entry(int x){return -x;}';exe=self.compile(source,'before',root);self.assertEqual(self.execute(exe,a=7),0xfffffff9)
   rules.write_text(original.replace('    neg t0, t0\\n','    not t0, t0\\n',1));exe=self.compile(source,'after',root);self.assertEqual(self.execute(exe,a=7),0xfffffff8)
   rules.write_text(original.replace('rule unary.neg.i32','rule removed.unary.neg.i32',1));path=OUT/'bad.c';path.write_text(source);p=subprocess.run([sys.executable,'tools/xhcc.py','--package-root',str(root),'variant-gcc','-c',str(path.relative_to(ROOT)),'-o',str((OUT/'bad.o').relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertNotEqual(p.returncode,0)
  self.assertEqual(hashlib.sha256(compiler.read_bytes()).digest(),digest)
if __name__=='__main__':unittest.main()
