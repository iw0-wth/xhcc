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
   root=Path(tmp);pkg=self.package(root,'x86_64');path=pkg/'x86.encoding.rules';base=path.read_text();outputs=[]
   self.assertIn('bits 0 0 1 3\n',base)
   for changed in [False,True]:
    path.write_text(base.replace('bits 0 0 1 3\n','') if changed else base)
    obj=OUT/('rex_changed.o' if changed else 'rex_base.o');self.invoke(root,'variant-gcc',source,obj);outputs.append(self.execute(obj,True))
   self.assertEqual(outputs,[2,0])
  self.assertEqual(hashlib.sha256(self.compiler.read_bytes()).digest(),before)
 def test_modrm_sib_known_bytes_and_external_mutation(self):
  before=hashlib.sha256(self.compiler.read_bytes()).digest()
  for target in ['x86','x86_64']:
   with self.subTest(target=target),tempfile.TemporaryDirectory(prefix='xh-x86-') as tmp:
    root=Path(tmp);pkg=self.package(root,target);path=pkg/'x86.encoding.rules';base=path.read_text();source=OUT/(target+'_fields.c')
    text=('mov eax, [ebp]\nmov edx, [esp+ecx*4+127]\nmov ebx, [ebp-128]\nmov edi, [eax+ecx*8+128]\n' if target=='x86' else 'mov r9, [r13]\nmov r10, [r12+r9*4+127]\nmov r11, [r13-128]\nmov r15, [r8+r9*8+128]\n')
    source.write_text('int entry(void){asm volatile("'+text.replace('\n','\\n')+'");return 0;}\n');obj=OUT/(target+'_fields.o');self.invoke(root,'variant-gcc',source,obj)
    with obj.open('rb') as f:actual=ELFFile(f).get_section_by_name('.text').data()
    # Stable Intel encodings cover forced disp8 at base5, SIB base4,
    # extended REX R/X/B, and displacement endpoints / full disp32.
    expected=bytes.fromhex('8b4500 8b548c7f 8b5d80 8bbc c880000000' if target=='x86' else '4d8b4d00 4f8b548c7f 4d8b5d80 4f8bbc c880000000')
    self.assertIn(expected,actual)
    source=OUT/'sib_exec.c';instructions='mov ecx, 0x710000\\nmov dword ptr [ecx+4], 5\\nmov dword ptr [ecx+8], 11\\nmov edx, 1\\nmov eax, [ecx+edx*4]' if target=='x86' else 'mov rcx, 0x710000\\nmov dword ptr [rcx+4], 5\\nmov dword ptr [rcx+8], 11\\nmov edx, 1\\nmov eax, [rcx+rdx*4]';source.write_text('int entry(void){int out;asm volatile("'+instructions+'" : "=r"(out));return out;}\n')
    outputs=[]
    start=base.index('rule SIB ');end=base.index('\nend',start)
    for changed in [False,True]:
     # Force scale4 (code2) to encode scale8 by moving its low bit.
     variant=base[:start]+base[start:end].replace('rule SIB 3 0','rule SIB 3 0x40').replace('bits 0 0 2 6','bits 0 1 1 7')+base[end:] if changed else base
     path.write_text(variant);obj=OUT/(target+('_sib_changed.o' if changed else '_sib_base.o'));self.invoke(root,'variant-gcc',source,obj);outputs.append(self.execute(obj,target=='x86_64'))
    self.assertEqual(outputs,[5,11])
    path.write_text('XHENC 999\n');obj=OUT/(target+'_field_reject.o')
    if obj.exists():obj.unlink()
    self.invoke(root,'variant-gcc',source,obj,ok=False)
  self.assertEqual(hashlib.sha256(self.compiler.read_bytes()).digest(),before)
 def test_absolute_and_index_only_addressing(self):
  for target in ['x86','x86_64']:
   with self.subTest(target=target),tempfile.TemporaryDirectory(prefix='xh-x86-') as tmp:
    root=Path(tmp);self.package(root,target);source=OUT/(target+'_nobase.c')
    instructions='mov dword ptr [0x710000], 7\\nmov dword ptr [0x710004], 19\\nmov edx, 1\\nmov eax, [0x710000]\\n'
    instructions+=('add eax, [edx*4+0x710000]' if target=='x86' else 'add eax, [rdx*4+0x710000]')
    source.write_text('int entry(void){int out;asm volatile("'+instructions+'" : "=r"(out));return out;}\n')
    obj=OUT/(target+'_nobase.o');self.invoke(root,'variant-gcc',source,obj);self.assertEqual(self.execute(obj,target=='x86_64'),26)
    with obj.open('rb') as f:code=ELFFile(f).get_section_by_name('.text').data()
    self.assertIn(bytes.fromhex('8b042500007100'),code)
    self.assertIn(bytes.fromhex('03049500007100'),code)
 def test_local_relocation_vm_legacy_and_modern_addends(self):
  import re
  before=hashlib.sha256(self.compiler.read_bytes()).digest()
  source=OUT/'local_refs.c';source.write_text('static int data[3]={7,13,29}; int *ptr=&data[1]; int entry(void){return *ptr+data[2];} int main(void){return entry();}\n')
  for target in ['x86','x86_64']:
   with self.subTest(target=target),tempfile.TemporaryDirectory(prefix='xh-local-reloc-') as tmp:
    root=Path(tmp);self.package(root,target);obj=OUT/(target+'_local_refs.o');self.invoke(root,'variant-gcc',source,obj);self.assertEqual(self.execute(obj,target=='x86_64'),42)
    # Native ELF keeps local references and explicit pointer addends for lld.
    with obj.open('rb') as f:
     elf=ELFFile(f);relocations=[r for sec in elf.iter_sections() if sec['sh_type'] in ['SHT_RELA','SHT_REL'] for r in sec.iter_relocations()]
     self.assertTrue(relocations)
    flat=OUT/(target+'_local_flat.bin');env=os.environ.copy();env['ARKCC_DUMP_SYMS']='1'
    command=[os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe'),'run','--dir=.','--env','ARKCC_DUMP_SYMS=1',str(self.compiler),'--ark-compat','-target',target,'-o',str(flat.relative_to(ROOT)),str(source.relative_to(ROOT))]
    p=subprocess.run(command,cwd=ROOT,env=env,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
    symbols={name:int(value,16) for name,value in re.findall(r'X86_SYM (\S+) 0x([0-9A-Fa-f]+)',p.stdout+p.stderr)}
    self.assertIn('entry',symbols);code=flat.read_bytes();wide=target=='x86_64';base=0x400000 if wide else 0
    uc=Uc(UC_ARCH_X86,UC_MODE_64 if wide else UC_MODE_32);uc.mem_map(base,0x40000);uc.mem_map(0x700000,0x20000);uc.mem_write(base,code);sp=0x71eff8 if wide else 0x71effc;stop=base+0x3f000
    uc.reg_write(UC_X86_REG_RSP if wide else UC_X86_REG_ESP,sp);uc.mem_write(sp,struct.pack('<Q' if wide else '<I',stop));uc.emu_start(base+symbols['entry'],stop,count=200000);self.assertEqual(uc.reg_read(UC_X86_REG_RAX if wide else UC_X86_REG_EAX)&0xffffffff,42)
    psz=8 if wide else 4;self.assertEqual(int.from_bytes(code[symbols['ptr']:symbols['ptr']+psz],'little'),base+symbols['data']+4)
    if wide:
     project=root/'flat_project';(project/'packages').mkdir(parents=True);shutil.copytree(ROOT/'packages'/target,project/'packages'/target);shutil.copy2(source,project/'input.c')
     rule=project/'packages'/target/'relocation.rules';text=rule.read_text();start=text.index('rule LOCAL_ABS64 ');end=text.index('\nend',start)
     text=text[:start]+text[start:end].replace('write r0 0 8','const r2 4294967296\nadd r0 r0 r2\nwrite r0 0 8')+text[end:];rule.write_text(text)
     changed=subprocess.run([command[0],'run','--dir=.',str(self.compiler),'--ark-compat','-target',target,'-o','changed.bin','input.c'],cwd=project,capture_output=True,text=True);self.assertEqual(changed.returncode,0,changed.stdout+changed.stderr)
     changed_code=(project/'changed.bin').read_bytes();self.assertEqual(int.from_bytes(changed_code[symbols['ptr']:symbols['ptr']+8],'little'),base+symbols['data']+4+0x100000000)
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
 def test_finite_abi_decision_table_equivalence_and_rejection(self):
  import re
  before=hashlib.sha256(self.compiler.read_bytes()).digest();source=OUT/'abi_decisions.c';source.write_text('double add(double a,int b){return a+b;} int entry(void){return (int)add(5.5,3);}\n')
  for target in ['x86','x86_64']:
   with self.subTest(target=target),tempfile.TemporaryDirectory(prefix='xh-abi-table-') as tmp:
    root=Path(tmp);pkg=self.package(root,target);path=pkg/'x86.rules';base=path.read_text();records=list(re.finditer(r'rule abi.decision\.(\d+)\ntext ([^\n]+)\nend\n',base));self.assertGreater(len(records),10)
    # Reorder disjoint first-match records: actual code generation must remain
    # equivalent while using the same parser and compiler image.
    values=[m[2] for m in records];reordered=base
    reordered=re.sub(r'(rule abi.decision\.)(\d+)(\ntext )([^\n]+)',lambda m:m[1]+m[2]+m[3]+values[-1-int(m[2])],base)
    for idx,text in enumerate([base,reordered]):
     path.write_text(text);obj=OUT/(target+f'_decision_eq{idx}.o');self.invoke(root,'variant-gcc',source,obj);self.assertEqual(self.execute(obj,target=='x86_64'),8)
    fp=next(m for m in records if m[2].split()[0:3]==['0','2','5']);fields=fp[2].split();fields[10]='0' if target=='x86_64' else '1';bad=base[:fp.start(2)]+' '.join(fields)+base[fp.end(2):]
    path.write_text(bad);obj=OUT/(target+'_decision_bad.o')
    if obj.exists():obj.unlink()
    self.invoke(root,'variant-gcc',source,obj,ok=False)
    path.write_text(base.replace('rule abi.decision.count\n','rule missing.abi.decision.count\n'));obj=OUT/(target+'_decision_missing.o')
    if obj.exists():obj.unlink()
    self.invoke(root,'variant-gcc',source,obj,ok=False)
  self.assertEqual(hashlib.sha256(self.compiler.read_bytes()).digest(),before)
 def test_fp_semantic_sequences_bits_and_same_sha_variant(self):
  source=OUT/'fp_semantic.c';source.write_text('''union F{float f;unsigned u;}; union D{double d;unsigned long long u;};
float fadd(float a,float b){return a+b;} double dadd(double a,double b){return a+b;}
int entry(void){union F f;union D d,n;float x=1.5f,y=2.0f;double a=7.5,b=2.5,z=0.0,mz=-0.0;unsigned u=4000000000u;
f.f=fadd(x,y);d.d=dadd(a,b);n.u=0x7ff8000000000001ULL;
return f.u==0x40600000u && d.u==0x4024000000000000ULL && (int)(a-b)==5 && (int)(a*b)==18 && (int)(a/b)==3
&& (int)(x-y)==0 && (int)(x*y)==3 && (int)(x/y)==0 && a>b && b<a && a>=b && b<=a && a!=b && !(a==b)
&& z==mz && !(z<mz) && !(z>mz) && n.d!=n.d && !(n.d==n.d) && !(n.d<z) && !(n.d<=z) && !(n.d>z) && !(n.d>=z)
&& (unsigned)(double)u==u && (unsigned)(float)u==u && (int)(double)-123==-123 && (int)(float)-123==-123 && (double)(float)1.5==1.5;}
''')
  before=hashlib.sha256(self.compiler.read_bytes()).digest()
  for target in ['x86','x86_64']:
   with self.subTest(target=target),tempfile.TemporaryDirectory(prefix='xh-fp-select-') as tmp:
    root=Path(tmp);pkg=self.package(root,target);obj=OUT/(target+'_fp_bits.o');self.invoke(root,'variant-gcc',source,obj);self.assertEqual(self.execute(obj,target=='x86_64'),1)
    path=pkg/'x86.rules';base=path.read_text();probe=OUT/'fp_variant.c';probe.write_text('double calc(double a,double b){return a+b;}int entry(void){return (int)calc(10.5,3.5);}\n');outputs=[]
    record='binary add 64 fp scalar fp.binary.add.64.fp.scalar 4 2'
    self.assertIn(record,base)
    for changed in [False,True]:
     path.write_text(base.replace(record,'binary add 64 fp scalar fp.binary.sub.64.fp.scalar 4 2') if changed else base)
     obj=OUT/(target+('_fp_changed.o' if changed else '_fp_base.o'));self.invoke(root,'variant-gcc',probe,obj);outputs.append(self.execute(obj,target=='x86_64'))
    self.assertEqual(outputs,[14,7])
    for idx,text in enumerate([base.replace(record,''),base.replace(record,record[:-3]+'3 2')]):
     path.write_text(text);obj=OUT/(target+f'_fp_reject{idx}.o')
     if obj.exists():obj.unlink()
     self.invoke(root,'variant-gcc',probe,obj,ok=False)
  self.assertEqual(hashlib.sha256(self.compiler.read_bytes()).digest(),before)
 def test_integer64_semantic_edges_and_rule_mutation(self):
  source=OUT/'integer64_semantic.c';source.write_text('''unsigned long long add(unsigned long long a,unsigned long long b){return a+b;}
unsigned long long sub(unsigned long long a,unsigned long long b){return a-b;}
unsigned long long mul(unsigned long long a,unsigned long long b){return a*b;}
unsigned long long left(unsigned long long a,int n){return a<<n;}
unsigned long long right(unsigned long long a,int n){return a>>n;}
long long aright(long long a,int n){return a>>n;}
int greater(long long a,long long b){return a>b;} int less(long long a,long long b){return a<b;}
int ge(long long a,long long b){return a>=b;} int le(long long a,long long b){return a<=b;}
long long sdiv(long long a,long long b){return a/b;} long long smod(long long a,long long b){return a%b;}
unsigned long long udiv(unsigned long long a,unsigned long long b){return a/b;} unsigned long long umod(unsigned long long a,unsigned long long b){return a%b;}
int entry(void){unsigned long long a=0x12345678ffffffffULL,b=0x100000003ULL;long long n=-0x123456789LL;int shifts[4]={0,31,32,63};int i;
if(add(a,b)!=0x1234567a00000002ULL || sub(a,b)!=0x12345677fffffffcULL || mul(0x100000001ULL,0x100000003ULL)!=0x400000003ULL)return 0;
for(i=0;i<4;i++){int k=shifts[i];if(left(1ULL,k)!=(1ULL<<k) || right(0x8000000000000000ULL,k)!=(0x8000000000000000ULL>>k) || aright(-8LL,k)!=(-8LL>>k))return 0;}
if(!greater(0x1ffffffffLL,0x100000000LL) || !ge(0x1ffffffffLL,0x100000000LL) || !less(0x100000000LL,0x1ffffffffLL) || !le(0x100000000LL,0x1ffffffffLL))return 0;
if(!(n<1LL) || n==1LL || !(0x8000000000000000ULL>1ULL) || 0x8000000000000000ULL<1ULL)return 0;
return udiv(0x123456789abcdef0ULL,17ULL)==0x112233445566777ULL && umod(0x123456789abcdef0ULL,17ULL)==9 && sdiv(-10000000001LL,3LL)==-3333333333LL && smod(-10000000001LL,3LL)==-2;}
''')
  before=hashlib.sha256(self.compiler.read_bytes()).digest()
  for target in ['x86','x86_64']:
   with self.subTest(target=target),tempfile.TemporaryDirectory(prefix='xh-i64-') as tmp:
    root=Path(tmp);pkg=self.package(root,target);obj=OUT/(target+'_integer64.o');self.invoke(root,'variant-gcc',source,obj);self.assertEqual(self.execute(obj,target=='x86_64'),1)
    probe=OUT/'integer64_variant.c';probe.write_text('long long calc(long long a,long long b){return a+b;}int entry(void){return (int)calc(0x10000000aLL,3LL);}\n');path=pkg/'x86.rules';base=path.read_text();record='binary add 64 signed pair int64.binary.add.signed 4 2';self.assertIn(record,base);outputs=[]
    for changed in [False,True]:
     path.write_text(base.replace(record,'binary add 64 signed pair int64.binary.sub.signed 4 2') if changed else base);obj=OUT/(target+('_i64_changed.o' if changed else '_i64_base.o'));self.invoke(root,'variant-gcc',probe,obj);outputs.append(self.execute(obj,target=='x86_64'))
    self.assertEqual(outputs,[13,7])
    path.write_text(base.replace(record,''));obj=OUT/(target+'_i64_rejected.o')
    if obj.exists():obj.unlink()
    self.invoke(root,'variant-gcc',probe,obj,ok=False)
  self.assertEqual(hashlib.sha256(self.compiler.read_bytes()).digest(),before)
 def test_integer64_float_conversions_and_unary(self):
  source=OUT/'wide_convert.c';source.write_text('''double sd(long long a){return (double)a;} double ud(unsigned long long a){return (double)a;}
float sf(long long a){return (float)a;} float uf(unsigned long long a){return (float)a;}
long long ds(double a){return (long long)a;} unsigned long long du(double a){return (unsigned long long)a;}
long long fs(float a){return (long long)a;} unsigned long long fu(float a){return (unsigned long long)a;}
long long neg(long long a){return -a;} unsigned long long inv(unsigned long long a){return ~a;} int zero(long long a){return !a;}
union Raw{unsigned long long bits;double d;};
int entry(void){long long a=-4294967297LL;unsigned long long u=9223372036854775808ULL;union Raw boundary,last;boundary.bits=0x43e0000000000000ULL;last.bits=0x43dfffffffffffffULL;
return sd(a)==-4294967297.0 && ud(u)==boundary.d && sf(-2147483648LL)==-2147483648.0f && uf(4294967296ULL)==4294967296.0f
&& ds(-4294967297.75)==-4294967297LL && du(boundary.d)==u && fs(-2147483648.0f)==-2147483648LL && fu(4294967296.0f)==4294967296ULL
&& ds(2147483647.0)==2147483647LL && ds(4294967296.0)==4294967296LL && ds(last.d)==9223372036854774784LL
&& neg(a)==4294967297LL && inv(0x123456789abcdef0ULL)==0xedcba9876543210fULL && zero(0LL)==1 && zero(1LL)==0 && zero(0x100000000LL)==0;}
''')
  before=hashlib.sha256(self.compiler.read_bytes()).digest()
  for target in ['x86','x86_64']:
   with self.subTest(target=target),tempfile.TemporaryDirectory(prefix='xh-wide-conv-') as tmp:
    root=Path(tmp);pkg=self.package(root,target);obj=OUT/(target+'_wide_convert.o');self.invoke(root,'variant-gcc',source,obj);self.assertEqual(self.execute(obj,target=='x86_64'),1)
    path=pkg/'x86.rules';base=path.read_text();record='convert i64tof 64 signed pair int64.convert.i64tof.64.signed 2 2';self.assertIn(record,base)
    probe=OUT/'wide_convert_variant.c';probe.write_text('double cv(long long x){return (double)x;}int entry(void){return cv(-1LL)<0.0;}\n');outputs=[]
    for changed in [False,True]:
     path.write_text(base.replace(record,'convert i64tof 64 signed pair int64.convert.i64tof.64.unsigned 2 2') if changed else base);obj=OUT/(target+('_conv_changed.o' if changed else '_conv_base.o'));self.invoke(root,'variant-gcc',probe,obj);outputs.append(self.execute(obj,target=='x86_64'))
    self.assertEqual(outputs,[1,0])
    path.write_text(base.replace(record,''));obj=OUT/(target+'_conv_rejected.o')
    if obj.exists():obj.unlink()
    self.invoke(root,'variant-gcc',probe,obj,ok=False)
  self.assertEqual(hashlib.sha256(self.compiler.read_bytes()).digest(),before)
 def test_exact_small_aggregate_load_at_page_boundary(self):
  # Compiler-generated argument/return loads must never read past the object.
  # The following page is deliberately unmapped in the actual CPU emulator.
  for size in [1,2,3,5,6,7]:
   with self.subTest(size=size):
    source=OUT/f'exact_guard{size}.c';fields=''.join(f'unsigned char c{i};' for i in range(size));checksum='+'.join(f'x.c{i}' for i in range(size))
    source.write_text(f'struct S{{{fields}}}; int take(struct S x){{return {checksum};}} int entry(void){{return take(*(struct S*){0x51000-size});}}\n')
    obj=source.with_suffix('.o');exe=source.with_suffix('.elf');p=subprocess.run([sys.executable,'tools/xhcc.py','x86_64-elf-gcc','-fuse-sysv-abi-plan','-O0','-c',str(source.relative_to(ROOT)),'-o',str(obj.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
    p=subprocess.run([str(SDK/'ld.lld.exe'),'-m','elf_x86_64','--image-base=0','-Ttext=0x10000','-e','entry','-o',str(exe),str(obj)],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
    with exe.open('rb') as f:
     elf=ELFFile(f);entry=elf.get_section_by_name('.symtab').get_symbol_by_name('entry')[0]['st_value'];uc=Uc(UC_ARCH_X86,UC_MODE_64);uc.mem_map(0x10000,0x30000);uc.mem_map(0x50000,0x1000);uc.mem_map(0x700000,0x20000)
     for sec in elf.iter_sections():
      if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
     uc.mem_write(0x51000-size,bytes(range(1,size+1)));uc.reg_write(UC_X86_REG_RSP,0x71eff8);uc.mem_write(0x71eff8,struct.pack('<Q',0x3f000));uc.emu_start(entry,0x3f000,count=200000);self.assertEqual(uc.reg_read(UC_X86_REG_RAX),size*(size+1)//2)
 def test_mov_word_operand_prefix_bytes_and_canary(self):
  for target in ['x86','x86_64']:
   with self.subTest(target=target):
    source=OUT/(target+'_word_mov.c');text='mov ecx, 0x20000\\nmov dword ptr [ecx], 0x77665544\\nmov ax, 0x1234\\nmov word ptr [ecx], ax\\nmov dx, word ptr [ecx]\\nmov ax, dx\\nmov eax, dword ptr [ecx]' if target=='x86' else 'mov rcx, 0x20000\\nmov dword ptr [rcx], 0x77665544\\nmov r8w, 0x1234\\nmov word ptr [rcx], r8w\\nmov r9w, word ptr [rcx]\\nmov r8w, r9w\\nmov eax, dword ptr [rcx]'
    source.write_text('int entry(void){int out;asm volatile("'+text+'" : "=r"(out));return out;}\n');obj=source.with_suffix('.o');p=subprocess.run([sys.executable,'tools/xhcc.py','i386-elf-gcc' if target=='x86' else 'x86_64-elf-gcc','-c',str(source.relative_to(ROOT)),'-o',str(obj.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
    self.assertEqual(self.execute(obj,target=='x86_64'),0x77661234)
    with obj.open('rb') as f:code=ELFFile(f).get_section_by_name('.text').data()
    expected=['668901','668b11','6689d0'] if target=='x86' else ['66448901','66448b09','664589c8']
    for value in expected:self.assertIn(bytes.fromhex(value),code)
 def test_branch_target_symbols_named_like_narrow_registers(self):
  source=OUT/'narrow_branch_symbols.c';source.write_text('int dl(void){return 3;}int al(void){return 5;}int r8b(void){return 7;}int entry(void){return dl()+al()+r8b();}\n')
  for target,tool in [('x86','i386-elf-gcc'),('x86_64','x86_64-elf-gcc')]:
   with self.subTest(target=target):
    obj=OUT/(target+'_narrow_symbols.o');p=subprocess.run([sys.executable,'tools/xhcc.py',tool,'-O0','-c',str(source.relative_to(ROOT)),'-o',str(obj.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr);self.assertEqual(self.execute(obj,target=='x86_64'),15)
    with obj.open('rb') as f:
     elf=ELFFile(f);symbols=elf.get_section_by_name('.symtab');targets=[]
     for sec in elf.iter_sections():
      if sec['sh_type'] in ['SHT_RELA','SHT_REL']:
       for rel in sec.iter_relocations():
        name=symbols.get_symbol(rel['r_info_sym']).name
        if name in ['dl','al','r8b']:targets.append(name)
     self.assertEqual(sorted(targets),['al','dl','r8b'])
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
