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
  cases=['struct S { double x; }; struct S f(struct S x) { return x; }',
         'struct S { long a,b; }; extern int f(struct S); int entry(void) { struct S x={1,2}; return f(x); }',
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
 def test_irregular_size_and_variadic_aggregate_are_rejected(self):
  cases=['struct S { char a,b,c; }; struct S f(struct S a) { return a; }',
         'struct S { char a,b,c,d,e; }; struct S f(struct S a) { return a; }',
         'struct S { int x; }; extern int f(int,...); int entry(void) { struct S a={3};return f(1,a); }']
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
   self.assertIn('stale plans',(p.stdout+p.stderr).lower())
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
