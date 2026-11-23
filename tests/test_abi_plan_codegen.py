"""Opt-in scalar SysV plan consumer: two TUs, real LLVM link + CPU execution.
No compiler build/install is performed. Optional native Clang mixed-toolchain
checks skip explicitly if CLANG_REFERENCE/an installed reference is unavailable.
"""
from pathlib import Path
import os, subprocess, sys, tempfile, unittest, shutil, struct
from test_driver import ROOT, command
import test_x86_rules

LIBRARY = '''
long integer8(long a,long b,long c,long d,long e,long f,long g,long h) { return a+b+c+d+e+f+g+h; }
double floating10(double a,double b,double c,double d,double e,double f,double g,double h,double i,double j) { return a+b+c+d+e+f+g+h+i+j; }
float single(float a,int b) { return a+b; }
long pointed(long *p) { return *p; }
#include <stdarg.h>
double variadic(int n, ...) { va_list ap; double a,b; long c; va_start(ap,n); a=va_arg(ap,double); c=va_arg(ap,long); b=va_arg(ap,double); va_end(ap); return a+b+c+n; }
'''
CALLER = '''
long integer8(long,long,long,long,long,long,long,long);
double floating10(double,double,double,double,double,double,double,double,double,double);
float single(float,int); long pointed(long *); double variadic(int,...);
int entry(void) { long x=17;
 return integer8(1,2,3,4,5,6,7,8)==36 && floating10(1,2,3,4,5,6,7,8,9,10)==55.0
 && single(1.5f,2)==3.5f && pointed(&x)==17 && variadic(3,1.5,4L,2.5)==11.0; }
'''

AGG_LIBRARY = '''
struct S4 { int x; }; struct S8 { int x; int y; }; struct P8 { long *p; };
struct S4 echo4(struct S4 a) { a.x += 3; return a; }
struct S8 echo8(struct S8 a) { a.x += 2; a.y += 5; return a; }
long consume4(struct S4 a, long n) { return a.x + n; }
long spill8(long a,long b,long c,long d,long e,long f,struct S8 x,long tail) { return a+b+c+d+e+f+x.x+x.y+tail; }
long pointer8(struct P8 a) { return *a.p; }
'''
AGG_CALLER = '''
struct S4 { int x; }; struct S8 { int x; int y; }; struct P8 { long *p; };
struct S4 echo4(struct S4); struct S8 echo8(struct S8);
long consume4(struct S4,long);long spill8(long,long,long,long,long,long,struct S8,long);long pointer8(struct P8);
int entry(void) { struct S4 a={7}, ar; struct S8 b={11,13}, br; long value=31; struct P8 p;
 ar=echo4(a);br=echo8(b);p.p=&value;
 return ar.x==10 && br.x==13 && br.y==18 && consume4(a,5)==12
 && spill8(1,2,3,4,5,6,b,9)==54 && pointer8(p)==31; }
'''

class AbiPlanCodegen(unittest.TestCase):
 def invoke(self,src,obj,*extra,ok=True):
  p=subprocess.run([*command(),'x86_64-linux-gnu-gcc','-O0','-fuse-sysv-abi-plan','-c',*extra,str(src.relative_to(ROOT)),'-o',str(obj.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True,timeout=60)
  self.assertEqual(p.returncode==0,ok,p.stdout+p.stderr)
  if not ok:self.assertFalse(obj.exists())
  return p
 def execute(self,objects,out):
  sdk=test_x86_rules.SDK
  p=subprocess.run([str(sdk/'ld.lld.exe'),'-m','elf_x86_64','-r',*map(str,objects),'-o',str(out)],cwd=ROOT,capture_output=True,text=True)
  self.assertEqual(p.returncode,0,p.stderr)
  return test_x86_rules.X86Rules().execute(out,True)
 def sources(self,work):
  library=work/'library.c';caller=work/'caller.c';library.write_text(LIBRARY);caller.write_text(CALLER);return library,caller
 def test_two_translation_units_scalar_fp_spill_and_varargs(self):
  with tempfile.TemporaryDirectory(prefix='abi-native-',dir=ROOT/'build') as tmp:
   work=Path(tmp);library,caller=self.sources(work);objects=[]
   for src in [library,caller]:
    obj=src.with_suffix('.o');self.invoke(src,obj);objects.append(obj)
   self.assertEqual(self.execute(objects,work/'combined.o'),1)
 def test_unsupported_aggregate_is_explicitly_rejected(self):
  cases=['struct S { float x; short y; }; struct S f(struct S x) { return x; }',
         'struct S { long a,b,c; }; extern int f(struct S); int entry(void) { struct S x={1,2,3}; return f(x); }',
         'struct __attribute__((packed)) S { int x; }; struct S f(struct S x) { return x; }',
         'struct S { unsigned x:4; }; struct S f(struct S x) { return x; }',
         'union S { int x; }; union S f(union S x) { return x; }',
         'struct S { int x[2]; }; struct S f(struct S x) { return x; }']
  with tempfile.TemporaryDirectory(prefix='abi-aggregate-',dir=ROOT/'build') as tmp:
   work=Path(tmp)
   for i,text in enumerate(cases):
    src=work/f'bad{i}.c';src.write_text(text+'\n');p=self.invoke(src,src.with_suffix('.o'),ok=False)
    self.assertIn('aggregate',(p.stdout+p.stderr).lower())
 def test_small_integer_struct_two_tu_and_clang_both_directions(self):
  clang=os.environ.get('CLANG_REFERENCE') or shutil.which('clang') or (str(Path(r'D:\msys64\ucrt64\bin\clang.exe')) if Path(r'D:\msys64\ucrt64\bin\clang.exe').is_file() else None)
  self.assertIsNotNone(clang,'reference Clang required for aggregate ABI acceptance')
  with tempfile.TemporaryDirectory(prefix='abi-small-aggregate-',dir=ROOT/'build') as tmp:
   work=Path(tmp);library=work/'library.c';caller=work/'caller.c';library.write_text(AGG_LIBRARY);caller.write_text(AGG_CALLER)
   for route in ['xhcc','clang-library','clang-caller']:
    objects=[]
    for src in [library,caller]:
     reference=(route=='clang-library' and src==library) or (route=='clang-caller' and src==caller)
     obj=work/(src.stem+'_'+route+'.o')
     if reference:
      p=subprocess.run([clang,'--target=x86_64-unknown-linux-gnu','-O0','-ffreestanding','-fno-stack-protector','-fno-pic','-c',str(src),'-o',str(obj)],cwd=ROOT,capture_output=True,text=True)
      self.assertEqual(p.returncode,0,p.stderr)
     else:self.invoke(src,obj)
     objects.append(obj)
    self.assertEqual(self.execute(objects,work/(route+'_combined.o')),1,route)
 def test_irregular_integer_struct_bytes_canaries_spill_and_clang(self):
  clang=os.environ.get('CLANG_REFERENCE') or str(Path(r'D:\msys64\ucrt64\bin\clang.exe'))
  self.assertTrue(Path(clang).is_file(),'reference Clang required')
  declarations=[];definitions=[];checks=[]
  for size in [1,2,3,5,6,7]:
   fields=' '.join(f'unsigned char b{i};' for i in range(size))
   declarations.append(f'struct S{size} {{ {fields} }};')
   definitions.append(f'struct S{size} echo{size}(struct S{size} x) {{ x.b0 += 1; return x; }}')
   expression='+'.join(f'x.b{i}' for i in range(size))
   definitions.append(f'long spill{size}(long a,long b,long c,long d,long e,long f,struct S{size} x,long tail) {{ return a+b+c+d+e+f+{expression}+tail; }}')
   checks.append(f'''{{ struct W{size} {{ unsigned char before; struct S{size} value; unsigned char after; }} w;
    struct S{size} input={{ {','.join(str(i+1) for i in range(size))} }};
    w.before=91;w.after=93;w.value=echo{size}(input);
    if(w.before!=91||w.after!=93||w.value.b0!=2||w.value.b{size-1}!={2 if size==1 else size})return {size};
    if(spill{size}(1,2,3,4,5,6,input,9)!={30+size*(size+1)//2})return {size+10}; }}''')
  prototypes=[]
  for size in [1,2,3,5,6,7]:
   prototypes.append(f'struct S{size} echo{size}(struct S{size}); long spill{size}(long,long,long,long,long,long,struct S{size},long);')
  header='\n'.join(declarations)+'\n'
  with tempfile.TemporaryDirectory(prefix='abi-irregular-',dir=ROOT/'build') as tmp:
   work=Path(tmp);library=work/'library.c';caller=work/'caller.c'
   library.write_text(header+'\n'.join(definitions)+'\n')
   caller.write_text(header+'\n'.join(prototypes)+'\nint entry(void) {'+'\n'.join(checks)+'return 0;}\n')
   for route in ['xhcc','clang-library','clang-caller']:
    objects=[]
    for src in [library,caller]:
     reference=(route=='clang-library' and src==library) or (route=='clang-caller' and src==caller)
     obj=work/(src.stem+'_'+route+'.o')
     if reference:
      p=subprocess.run([clang,'--target=x86_64-unknown-linux-gnu','-O0','-ffreestanding','-fno-stack-protector','-fno-pic','-c',str(src),'-o',str(obj)],cwd=ROOT,capture_output=True,text=True)
      self.assertEqual(p.returncode,0,p.stderr)
     else:self.invoke(src,obj)
     objects.append(obj)
    self.assertEqual(self.execute(objects,work/(route+'_combined.o')),0,route)
 def test_small_sse_struct_clang_routes_fp_spill_gpr_mix_canaries(self):
  clang=os.environ.get('CLANG_REFERENCE') or str(Path(r'D:\msys64\ucrt64\bin\clang.exe'))
  self.assertTrue(Path(clang).is_file(),'reference Clang required')
  header='struct F4 { float x; }; struct F8 { float x,y; }; struct D8 { double x; };\n'
  library_text=header+'''
struct F4 echo_f4(struct F4 x) { x.x += 1.0f; return x; }
struct F8 echo_f8(struct F8 x) { x.x += 2.0f; x.y += 3.0f; return x; }
struct D8 echo_d8(struct D8 x) { x.x += 4.0; return x; }
double spill_f8(double a,double b,double c,double d,double e,double f,double g,double h,struct F8 v,long n) { return a+b+c+d+e+f+g+h+v.x+v.y+n; }
double mix_sse(long a,struct F4 b,long c,struct F8 d,struct D8 e) { return a+b.x+c+d.x+d.y+e.x; }
'''
  caller_text=header+'''
struct F4 echo_f4(struct F4); struct F8 echo_f8(struct F8); struct D8 echo_d8(struct D8);
double spill_f8(double,double,double,double,double,double,double,double,struct F8,long);
double mix_sse(long,struct F4,long,struct F8,struct D8);
int entry(void) {
 struct WF4 { int before; struct F4 value; int after; } a;
 struct WF8 { int before; struct F8 value; int after; } b;
 struct WD8 { long before; struct D8 value; long after; } c;
 struct F4 in4={1.5f}; struct F8 in8={2.5f,3.5f}; struct D8 inD={4.5};
 a.before=71;a.after=73;b.before=75;b.after=77;c.before=79;c.after=81;
 a.value=echo_f4(in4);b.value=echo_f8(in8);c.value=echo_d8(inD);
 if(a.before!=71||a.after!=73||b.before!=75||b.after!=77||c.before!=79||c.after!=81)return 1;
 if(a.value.x!=2.5f||b.value.x!=4.5f||b.value.y!=6.5f||c.value.x!=8.5)return 2;
 if(spill_f8(1,2,3,4,5,6,7,8,in8,9)!=51.0)return 3;
 if(mix_sse(1,in4,2,in8,inD)!=15.0)return 4;
 return 0;
}
'''
  with tempfile.TemporaryDirectory(prefix='abi-sse-aggregate-',dir=ROOT/'build') as tmp:
   work=Path(tmp);library=work/'library.c';caller=work/'caller.c';library.write_text(library_text);caller.write_text(caller_text)
   for route in ['xhcc','clang-library','clang-caller']:
    objects=[]
    for src in [library,caller]:
     reference=(route=='clang-library' and src==library) or (route=='clang-caller' and src==caller)
     obj=work/(src.stem+'_'+route+'.o')
     if reference:
      p=subprocess.run([clang,'--target=x86_64-unknown-linux-gnu','-O0','-ffreestanding','-fno-stack-protector','-fno-pic','-c',str(src),'-o',str(obj)],cwd=ROOT,capture_output=True,text=True)
      self.assertEqual(p.returncode,0,p.stderr)
     else:self.invoke(src,obj)
     objects.append(obj)
    self.assertEqual(self.execute(objects,work/(route+'_combined.o')),0,route)
 def test_integer_pair16_clang_routes_rollback_returns_and_canaries(self):
  clang=os.environ.get('CLANG_REFERENCE') or str(Path(r'D:\msys64\ucrt64\bin\clang.exe'))
  self.assertTrue(Path(clang).is_file(),'reference Clang required')
  header='struct Pair16 { long a,b; };\n'
  library_text=header+'''
struct Pair16 make_pair(long n) { struct Pair16 p; p.a=n; p.b=n+10; return p; }
struct Pair16 edit_pair(struct Pair16 p) { p.a+=2; p.b+=3; return p; }
long rollback_pair(long a,long b,long c,long d,long e,struct Pair16 p,long tail) { return a+b+c+d+e+p.a+p.b+tail; }
'''
  caller_text=header+'''
struct Pair16 make_pair(long); struct Pair16 edit_pair(struct Pair16);
long rollback_pair(long,long,long,long,long,struct Pair16,long);
int entry(void) {
 struct Guard { long before; struct Pair16 p; long after; } g;
 struct Pair16 a,b,c,d;
 g.before=91;g.after=93;g.p=make_pair(5);
 if(g.before!=91||g.after!=93||g.p.a!=5||g.p.b!=15)return 1;
 a=make_pair(7);b=make_pair(11);c=edit_pair(a);d=edit_pair(make_pair(13));
 if(a.a!=7||a.b!=17||b.a!=11||b.b!=21)return 2;
 if(c.a!=9||c.b!=20||d.a!=15||d.b!=26)return 3;
 if(rollback_pair(1,2,3,4,5,a,19)!=58)return 4;
 return 0;
}
'''
  with tempfile.TemporaryDirectory(prefix='abi-intpair-',dir=ROOT/'build') as tmp:
   work=Path(tmp);library=work/'library.c';caller=work/'caller.c';library.write_text(library_text);caller.write_text(caller_text)
   for route in ['xhcc','clang-library','clang-caller']:
    objects=[]
    for src in [library,caller]:
     reference=(route=='clang-library' and src==library) or (route=='clang-caller' and src==caller)
     obj=work/(src.stem+'_'+route+'.o')
     if reference:
      p=subprocess.run([clang,'--target=x86_64-unknown-linux-gnu','-O0','-ffreestanding','-fno-stack-protector','-fno-pic','-c',str(src),'-o',str(obj)],cwd=ROOT,capture_output=True,text=True)
      self.assertEqual(p.returncode,0,p.stderr)
     else:self.invoke(src,obj)
     objects.append(obj)
    self.assertEqual(self.execute(objects,work/(route+'_combined.o')),0,route)
 def test_mixed_pair16_clang_routes_independent_bank_rollback(self):
  clang=os.environ.get('CLANG_REFERENCE') or str(Path(r'D:\msys64\ucrt64\bin\clang.exe'))
  self.assertTrue(Path(clang).is_file(),'reference Clang required')
  header='struct LD { long a; double b; }; struct DL { double a; long b; }; struct DD { double a,b; };\n'
  library_text=header+'''
struct LD ld(long n) { struct LD p; p.a=n; p.b=n+0.5; return p; }
struct DL dl(long n) { struct DL p; p.a=n+0.25; p.b=n+10; return p; }
struct DD dd(long n) { struct DD p; p.a=n+0.5;p.b=n+1.5;return p; }
double take_ld(struct LD p) { return p.a+p.b; }
double take_dl(struct DL p) { return p.a+p.b; }
double take_dd(struct DD p) { return p.a+p.b; }
double gp_full(long a,long b,long c,long d,long e,long f,struct LD p,double tail) { return a+b+c+d+e+f+p.a+p.b+tail; }
double fp_full(double a,double b,double c,double d,double e,double f,double g,double h,struct DL p,long tail) { return a+b+c+d+e+f+g+h+p.a+p.b+tail; }
double sse_pair_rollback(double a,double b,double c,double d,double e,double f,double g,struct DD p,double tail) { return a+b+c+d+e+f+g+p.a+p.b+tail; }
'''
  caller_text=header+'''
struct LD ld(long);struct DL dl(long);struct DD dd(long);
double take_ld(struct LD);double take_dl(struct DL);double take_dd(struct DD);
double gp_full(long,long,long,long,long,long,struct LD,double);
double fp_full(double,double,double,double,double,double,double,double,struct DL,long);
double sse_pair_rollback(double,double,double,double,double,double,double,struct DD,double);
int entry(void) {
 struct GuardLD { long before;struct LD p;long after; } a;
 struct GuardDL { long before;struct DL p;long after; } b;
 struct GuardDD { long before;struct DD p;long after; } c;
 struct LD a2;struct DL b2;struct DD c2;
 a.before=91;a.after=93;b.before=95;b.after=97;c.before=99;c.after=101;
 a.p=ld(3);b.p=dl(4);c.p=dd(5);a2=ld(7);b2=dl(8);c2=dd(9);
 if(a.before!=91||a.after!=93||b.before!=95||b.after!=97||c.before!=99||c.after!=101)return 1;
 if(a.p.a!=3||a.p.b!=3.5||b.p.a!=4.25||b.p.b!=14||c.p.a!=5.5||c.p.b!=6.5)return 2;
 if(a2.a!=7||a2.b!=7.5||b2.a!=8.25||b2.b!=18||c2.a!=9.5||c2.b!=10.5)return 3;
 if(take_ld(a.p)!=6.5||take_dl(b.p)!=18.25||take_dd(c.p)!=12.0)return 4;
 if(gp_full(1,2,3,4,5,6,a.p,2.5)!=30.0)return 5;
 if(fp_full(1,2,3,4,5,6,7,8,b.p,9)!=63.25)return 6;
 if(sse_pair_rollback(1,2,3,4,5,6,7,c.p,8)!=48.0)return 7;
 return 0;
}
'''
  with tempfile.TemporaryDirectory(prefix='abi-mixed-pair-',dir=ROOT/'build') as tmp:
   work=Path(tmp);library=work/'library.c';caller=work/'caller.c';library.write_text(library_text);caller.write_text(caller_text)
   for route in ['xhcc','clang-library','clang-caller']:
    objects=[]
    for src in [library,caller]:
     reference=(route=='clang-library' and src==library) or (route=='clang-caller' and src==caller)
     obj=work/(src.stem+'_'+route+'.o')
     if reference:
      p=subprocess.run([clang,'--target=x86_64-unknown-linux-gnu','-O0','-ffreestanding','-fno-stack-protector','-fno-pic','-c',str(src),'-o',str(obj)],cwd=ROOT,capture_output=True,text=True)
      self.assertEqual(p.returncode,0,p.stderr)
     else:self.invoke(src,obj)
     objects.append(obj)
    self.assertEqual(self.execute(objects,work/(route+'_combined.o')),0,route)
 def test_single_slot_int_float_struct_clang_routes_raw_bits_and_spill(self):
  clang=os.environ.get('CLANG_REFERENCE') or str(Path(r'D:\msys64\ucrt64\bin\clang.exe'))
  self.assertTrue(Path(clang).is_file(),'reference Clang required')
  header='struct IF { int i; float f; }; struct FI { float f; int i; };\n'
  library_text=header+'''
struct IF identity_if(struct IF x) { return x; }
struct FI identity_fi(struct FI x) { return x; }
long fit_last(long a,long b,long c,long d,long e,struct IF x,long tail) { return a+b+c+d+e+x.i+tail; }
long spill_slot(long a,long b,long c,long d,long e,long f,struct FI x,long tail) { return a+b+c+d+e+f+x.i+tail; }
'''
  caller_text=header+'''
struct IF identity_if(struct IF); struct FI identity_fi(struct FI);
long fit_last(long,long,long,long,long,struct IF,long);
long spill_slot(long,long,long,long,long,long,struct FI,long);
union Bits { float f; unsigned int u; };
int entry(void) {
 struct GuardIF { int before;struct IF p;int after; } a;
 struct GuardFI { int before;struct FI p;int after; } b;
 struct IF input_if,second_if;struct FI input_fi,second_fi;union Bits bits,readback;
 bits.u=0x80000000U;input_if.i=-17;input_if.f=bits.f;
 bits.u=0x7fc12345U;input_fi.i=23;input_fi.f=bits.f;
 a.before=91;a.after=93;b.before=95;b.after=97;
 a.p=identity_if(input_if);b.p=identity_fi(input_fi);
 second_if=identity_if(input_if);second_fi=identity_fi(input_fi);
 if(a.before!=91||a.after!=93||b.before!=95||b.after!=97)return 1;
 if(a.p.i!=-17||b.p.i!=23||second_if.i!=-17||second_fi.i!=23)return 2;
 readback.f=a.p.f;if(readback.u!=0x80000000U)return 3;
 readback.f=b.p.f;if(readback.u!=0x7fc12345U)return 4;
 readback.f=second_if.f;if(readback.u!=0x80000000U)return 5;
 readback.f=second_fi.f;if(readback.u!=0x7fc12345U)return 6;
 if(fit_last(1,2,3,4,5,input_if,31)!=29)return 7;
 if(spill_slot(1,2,3,4,5,6,input_fi,29)!=73)return 8;
 return 0;
}
'''
  with tempfile.TemporaryDirectory(prefix='abi-single-mixed-',dir=ROOT/'build') as tmp:
   work=Path(tmp);library=work/'library.c';caller=work/'caller.c';library.write_text(library_text);caller.write_text(caller_text)
   for route in ['xhcc','clang-library','clang-caller']:
    objects=[]
    for src in [library,caller]:
     reference=(route=='clang-library' and src==library) or (route=='clang-caller' and src==caller)
     obj=work/(src.stem+'_'+route+'.o')
     if reference:
      p=subprocess.run([clang,'--target=x86_64-unknown-linux-gnu','-O0','-ffreestanding','-fno-stack-protector','-fno-pic','-c',str(src),'-o',str(obj)],cwd=ROOT,capture_output=True,text=True)
      self.assertEqual(p.returncode,0,p.stderr)
     else:self.invoke(src,obj)
     objects.append(obj)
    self.assertEqual(self.execute(objects,work/(route+'_combined.o')),0,route)
 def test_private_ir_lto_reload_pair16_executes_and_rejects_bad_context(self):
  clang=os.environ.get('CLANG_REFERENCE') or str(Path(r'D:\msys64\ucrt64\bin\clang.exe'))
  self.assertTrue(Path(clang).is_file(),'full target Clang required')
  header='struct Pair { long a,b; }; struct Mix { long a; double b; };\n'
  library_text=header+'struct Pair pair(struct Pair p){p.a+=2;p.b+=3;return p;} struct Mix mix(struct Mix p){p.a+=4;p.b+=0.5;return p;}\n'
  caller_text=header+'struct Pair pair(struct Pair);struct Mix mix(struct Mix); int entry(void){struct Pair a={7,11},b;struct Mix c={13,2.5},d;b=pair(a);d=mix(c);return b.a==9&&b.b==14&&d.a==17&&d.b==3.0&&a.a==7&&c.a==13;}\n'
  with tempfile.TemporaryDirectory(prefix='abi-lto-native-',dir=ROOT/'build') as tmp:
   work=Path(tmp);objects=[]
   for name,text in [('library',library_text),('caller',caller_text)]:
    src=work/(name+'.c');src.write_text(text);obj=src.with_suffix('.o');self.invoke(src,obj,'--private-object','--obj-ir');objects.append(obj)
   output=work/'reload.s'
   args=[*command(),'--ark-compat','-target','x86_64','-fuse-sysv-abi-plan','--lto','-link',*[str(p.relative_to(ROOT)) for p in objects],'-O0','-S','-o',str(output.relative_to(ROOT))]
   p=subprocess.run(args,cwd=ROOT,capture_output=True,text=True,timeout=60);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
   native=work/'reload.o'
   p=subprocess.run([clang,'--target=x86_64-unknown-linux-gnu','-c',str(output),'-o',str(native)],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
   self.assertEqual(self.execute([native],work/'combined.o'),1)
   # Same real payload, incompatible engine/mode: never silently use its address wires.
   for target,use in [('arm',False),('x86_64',False)]:
    badout=work/(target+'_no_use.s')
    badargs=[*command(),'--ark-compat','-target',target,'--lto','-link',*[str(p.relative_to(ROOT)) for p in objects],'-O0','-S','-o',str(badout.relative_to(ROOT))]
    p=subprocess.run(badargs,cwd=ROOT,capture_output=True,text=True,timeout=60)
    self.assertNotEqual(p.returncode,0,p.stdout+p.stderr);self.assertFalse(badout.exists())
    self.assertTrue('plan' in (p.stdout+p.stderr).lower() or 'aggregate' in (p.stdout+p.stderr).lower(),p.stdout+p.stderr)
   for invalid in [3,64]:
    data=bytearray(objects[0].read_bytes());start=data.index(b'ARKIR1\n');count=struct.unpack_from('<I',data,start+16)[0];cursor=start+20
    for _ in range(count):
     length=struct.unpack_from('<I',data,cursor)[0];cursor+=4;self.assertEqual(struct.unpack_from('<I',data,cursor+4)[0],2);struct.pack_into('<I',data,cursor+28,invalid);cursor+=length
    corrupt=work/f'flags{invalid}.o';corrupt.write_bytes(data);badout=work/f'flags{invalid}.s'
    p=subprocess.run([*command(),'--ark-compat','-target','x86_64','-fuse-sysv-abi-plan','--lto','-link',str(corrupt.relative_to(ROOT)),str(objects[1].relative_to(ROOT)),'-O0','-S','-o',str(badout.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True,timeout=60)
    self.assertNotEqual(p.returncode,0,p.stdout+p.stderr);self.assertFalse(badout.exists());self.assertIn('ABI plan',(p.stdout+p.stderr))
 def test_function_plan_references_match_source_signature_semantically(self):
  clang=os.environ.get('CLANG_REFERENCE') or str(Path(r'D:\msys64\ucrt64\bin\clang.exe'))
  self.assertTrue(Path(clang).is_file(),'full target Clang required')
  def references(data,name):
   encoded=name.encode();needle=struct.pack('<II',1,len(encoded))+encoded;start=data.index(b'ARKIR1\n');cursor=start+20
   plans=struct.unpack_from('<I',data,start+16)[0]
   for _ in range(plans):cursor+=4+struct.unpack_from('<I',data,cursor)[0]
   def string_at(pos):
    present=struct.unpack_from('<I',data,pos)[0];pos+=4
    if present==0:return pos
    if present!=1:raise ValueError('not a string')
    length=struct.unpack_from('<I',data,pos)[0];pos+=4
    if length>len(data)-pos:raise ValueError('string bounds')
    return pos+length
   pos=data.find(needle,cursor)
   while pos>=0:
    try:
     p=pos+len(needle);count=struct.unpack_from('<I',data,p)[0];p+=4
     if count>10000:raise ValueError('code count')
     for _ in range(count):
      op=struct.unpack_from('<I',data,p)[0];p+=12
      if op>100:raise ValueError('opcode')
      p=string_at(p);p+=16
     p+=22*4;p=string_at(p);p=string_at(p)
     count=struct.unpack_from('<I',data,p)[0];p+=4
     if count>64:raise ValueError('param names')
     for _ in range(count):p=string_at(p)
     count=struct.unpack_from('<I',data,p)[0];p+=4
     if count>64:raise ValueError('param types')
     p+=4*count+4
     a,b=struct.unpack_from('<II',data,p)
     if not(0<a<=plans and 0<b<=plans):raise ValueError('plan refs')
     return p,a,b
    except (ValueError,struct.error):pos=data.find(needle,pos+1)
   raise AssertionError('function record not located: '+name)
  with tempfile.TemporaryDirectory(prefix='abi-signature-',dir=ROOT/'build') as tmp:
   work=Path(tmp);src=work/'signature.c';obj=work/'signature.o'
   src.write_text('long f_src(long x){return x+1;} double g_src(double x){return x+0.5;} long h_src(long x){return x+2;} int entry(void){return f_src(4)==5 && g_src(2.0)==2.5;}\n')
   self.invoke(src,obj,'--private-object','--obj-ir');original=obj.read_bytes()
   fpos,fa,fr=references(original,'f_src');_,ga,gr=references(original,'g_src');_,ha,hr=references(original,'h_src')
   self.assertNotEqual(fa,ha,'distinct equal-content plan indices required')
   for mode in ['original','equivalent','wrong-parameter','wrong-result']:
    data=bytearray(original)
    if mode=='equivalent':struct.pack_into('<II',data,fpos,ha,hr)
    elif mode=='wrong-parameter':struct.pack_into('<I',data,fpos,ga)
    elif mode=='wrong-result':struct.pack_into('<I',data,fpos+4,gr)
    payload=work/(mode+'.o');payload.write_bytes(data);asm=work/(mode+'.s')
    p=subprocess.run([*command(),'--ark-compat','-target','x86_64','-fuse-sysv-abi-plan','--lto','-link',str(payload.relative_to(ROOT)),'-O0','-S','-o',str(asm.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True,timeout=60)
    if mode.startswith('wrong'):
     self.assertNotEqual(p.returncode,0,p.stdout+p.stderr);self.assertFalse(asm.exists());self.assertIn('function plan disagrees with source signature',p.stdout+p.stderr)
    else:
     self.assertEqual(p.returncode,0,p.stdout+p.stderr);native=work/(mode+'_native.o')
     p=subprocess.run([clang,'--target=x86_64-unknown-linux-gnu','-c',str(asm),'-o',str(native)],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
     self.assertEqual(self.execute([native],work/(mode+'_combined.o')),1)
 def test_variadic_aggregate_is_rejected(self):
  cases=['struct S { int x; }; extern int f(int,...); int entry(void) { struct S a={3};return f(1,a); }']
  with tempfile.TemporaryDirectory(prefix='abi-small-reject-',dir=ROOT/'build') as tmp:
   work=Path(tmp)
   for i,text in enumerate(cases):
    src=work/f'bad{i}.c';src.write_text(text+'\n');p=self.invoke(src,src.with_suffix('.o'),ok=False)
    self.assertIn('aggregate',(p.stdout+p.stderr).lower())
 def test_lto_rejects_changed_plan_rule_fingerprint(self):
  with tempfile.TemporaryDirectory(prefix='abi-fingerprint-',dir=ROOT/'build') as tmp:
   work=Path(tmp);src=work/'record.c';obj=work/'record.o'
   src.write_text('extern long add(long,long); long entry(void) { return add(3,4); }\n')
   self.invoke(src,obj,'--private-object','--obj-ir')
   original=obj.read_bytes();data=bytearray(original);start=data.index(b'ARKIR1\n')
   self.assertEqual(struct.unpack_from('<I',data,start+8)[0],2)
   count=struct.unpack_from('<I',data,start+16)[0];self.assertGreater(count,0)
   cursor=start+20
   for _ in range(count):
    length=struct.unpack_from('<I',data,cursor)[0];cursor+=4
    self.assertEqual(data[cursor:cursor+4],b'XHAP')
    old=struct.unpack_from('<I',data,cursor+12)[0]
    struct.pack_into('<I',data,cursor+12,old^0x10001);cursor+=length
   bad=work/'wrong.o';bad.write_bytes(data);out=work/'wrong.s'
   p=subprocess.run([*command(),'--ark-compat','-target','x86_64','-fuse-sysv-abi-plan',
                     '--lto','-link',str(bad.relative_to(ROOT)),'-S','-o',str(out.relative_to(ROOT))],
                    cwd=ROOT,capture_output=True,text=True,timeout=60)
   self.assertNotEqual(p.returncode,0,p.stdout+p.stderr)
   self.assertFalse(out.exists())
   diagnostics=(p.stdout+p.stderr).lower()
   self.assertIn('stale plans',diagnostics)
   for unrelated in ['no templates configured','file missing','missing or oversized','cannot open']:
    self.assertNotIn(unrelated,diagnostics)
 def test_mixed_reference_clang_both_directions(self):
  clang=os.environ.get('CLANG_REFERENCE') or shutil.which('clang') or (str(Path(r'D:\msys64\ucrt64\bin\clang.exe')) if Path(r'D:\msys64\ucrt64\bin\clang.exe').is_file() else None)
  if not clang:self.skipTest('native reference Clang unavailable; set CLANG_REFERENCE')
  with tempfile.TemporaryDirectory(prefix='abi-reference-',dir=ROOT/'build') as tmp:
   work=Path(tmp);library,caller=self.sources(work)
   for reference_library in [True,False]:
    objects=[]
    for src,reference in [(library,reference_library),(caller,not reference_library)]:
     obj=work/(src.stem+('_reference' if reference else '_xhcc')+'.o')
     if reference:
      p=subprocess.run([clang,'--target=x86_64-unknown-linux-gnu','-O0','-ffreestanding','-fno-stack-protector','-fno-pic','-c',str(src),'-o',str(obj)],cwd=ROOT,capture_output=True,text=True)
      self.assertEqual(p.returncode,0,p.stderr)
     else:self.invoke(src,obj)
     objects.append(obj)
    self.assertEqual(self.execute(objects,work/('mixed_library.o' if reference_library else 'mixed_caller.o')),1)
if __name__=='__main__':unittest.main()
