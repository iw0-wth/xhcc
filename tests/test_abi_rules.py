"""Compile only the small shared ABI/template VM harness; no compiler build.
Tests finite matching, exact field bounds, actual loaded x86 tables and cache
invalidation in a single Wasm process. Engine has no x86 wire policy.
"""
from pathlib import Path
import os,sys,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/abi-rule-tests'
SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))
WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
HARNESS=r'''
#include "xh_abi.h"
#include "xh_template.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int fs_file_size(const char*p){FILE*f=fopen(p,"rb");long n;if(!f)return -1;fseek(f,0,SEEK_END);n=ftell(f);fclose(f);return(int)n;}
int fs_read_file(const char*p,char*b,int cap){FILE*f=fopen(p,"rb");int n;if(!f)return -1;n=(int)fread(b,1,cap-1,f);b[n]=0;fclose(f);return n;}
void diag_tool(int s,const char*w,const char*m,const char*a,const char*b,const char*c,const char*d,const char*e,const char*f){fprintf(stderr,"%s: %s\n",w,m);}
static int query(const char*t,const char*prefix,XhAbiInput*in){XhAbiResult out={9,9,9,9,9};int n=xh_abi_select(t,prefix,in,&out);printf("%d %d %d %d %d %d\n",n,out.bank,out.words,out.width,out.alignment,out.indirect);return n;}
int main(int argc,char**argv){XhAbiInput in;int i;if(argc!=10)return 2;in.role=atoi(argv[3]);in.kind=atoi(argv[4]);in.size=atoi(argv[5]);in.alignment=atoi(argv[6]);in.variadic=atoi(argv[7]);in.index=atoi(argv[8]);if(xh_template_set_path("probe",argv[1]))return 2;
 if(!strcmp(argv[9],"single")){query("probe",argv[2],&in);return 0;}
 /* Switch path, target and prefix repeatedly in one process. */
 if(!strcmp(argv[9],"switch")){query("probe",argv[2],&in);xh_template_set_path("probe",argv[1]+0);xh_abi_release();query("probe",argv[2],&in);xh_template_set_path("other",argv[1]);query("other",argv[2],&in);query("probe","alternate",&in);query("probe",argv[2],&in);return 0;}
 if(!strcmp(argv[9],"path")){char path[512];query("probe",argv[2],&in);snprintf(path,sizeof(path),"%s.next",argv[1]);xh_template_set_path("probe",path);query("probe",argv[2],&in);xh_template_set_path("probe",argv[1]);query("probe",argv[2],&in);return 0;}
 if(!strcmp(argv[9],"mutate")){FILE*f;query("probe",argv[2],&in);f=fopen(argv[1],"wb");if(!f)return 2;fputs("XHTEMPLATE 1\nrule abi.decision.count\ntext 1\nend\nrule abi.decision.0\ntext 0 1 1 8 1 8 0 1 0 9 2 1 8 8 0 0\nend\n",f);fclose(f);xh_abi_release();query("probe",argv[2],&in);xh_template_release();query("probe",argv[2],&in);return 0;}
 return 2;}
'''
def table(records,prefix='abi.decision'):
 return 'XHTEMPLATE 1\n'+f'rule {prefix}.count\ntext {len(records)}\nend\n'+''.join(f'rule {prefix}.{i}\ntext {r}\nend\n' for i,r in enumerate(records))
R='0 1 1 8 1 8 0 1 0 9 0 2 8 8 0 0'
class AbiRules(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  OUT.mkdir(parents=True,exist_ok=True);(OUT/'harness.c').write_text(HARNESS)
  env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),env.get('PATH','')])
  p=subprocess.run([str(SDK/'bin/clang.exe'),'--target=wasm32-wasip1','-O2','-Iinclude','-include','include/xh_small.h','build/abi-rule-tests/harness.c','src/backend/rules/abi.c','src/backend/rules/template.c','-o','build/abi-rule-tests/vm.wasm'],cwd=ROOT,env=env,capture_output=True,text=True)
  if p.returncode:raise RuntimeError(p.stderr)
 def query(self,path,args=(0,1,4,4,0,0),mode='single',prefix='abi.decision'):
  p=subprocess.run([WASM,'run','--dir=.','build/abi-rule-tests/vm.wasm',str(path),prefix,*map(str,args),mode],cwd=ROOT,capture_output=True,text=True)
  self.assertEqual(p.returncode,0,p.stderr);return [tuple(map(int,line.split())) for line in p.stdout.splitlines()]
 def fixture(self,text):
  path=OUT/'fixture.rules';path.write_text(text);return path.relative_to(ROOT)
 def test_all_input_fields_and_first_match(self):
  path=self.fixture(table([R,R.replace(' 0 2 8 8 0 0',' 1 1 4 4 0 0')]))
  self.assertEqual(self.query(path),[(1,0,2,8,8,0)])
  for args in [(1,1,4,4,0,0),(0,2,4,4,0,0),(0,1,9,4,0,0),(0,1,4,9,0,0),(0,1,4,4,1,10)]:self.assertEqual(self.query(path,args),[(0,0,0,0,0,0)])
  narrow='1 4 9 16 8 16 1 1 3 4 2 1 8 8 1 0'
  self.assertEqual(self.query(self.fixture(table([narrow])),(1,4,12,8,1,3)),[(1,2,1,8,8,1)])
 def test_real_x86_records(self):
  self.assertEqual(self.query('packages/x86/x86.rules',(0,2,8,8,0,0)),[(1,2,2,8,4,0)])
  self.assertEqual(self.query('packages/x86_64/x86.rules',(0,2,8,8,0,0)),[(1,1,2,8,8,0)])
 def test_invalid_table_and_input_clear_outputs(self):
  bad=[R+' 0',R[:-1],R.replace('0 1 1 8','0 1 9 8'),R.replace('0 1 1 8','0 5 1 8'),R.replace(' 0 2 8 8 0 0',' 3 2 8 8 0 0'),R.replace(' 0 2 8 8 0 0',' 0 3 8 8 0 0'),R+'garbage',R.replace(' 0 2 8 8 0 0',' 0 2 8 8 0 1'),R.replace('0 1 1 8','0 1 65536 65536'),R.replace('0 1 1 8','0 1 +1 8')]
  for r in bad:self.assertEqual(self.query(self.fixture(table([r]))),[(-1,0,0,0,0,0)],r)
  for args in [(2,1,4,4,0,0),(0,1,4,0,0,0),(0,1,4,4,2,0),(0,1,4,4,0,-1)]:self.assertEqual(self.query(self.fixture(table([R])),args),[(-1,0,0,0,0,0)])
  for count in [0,33]:self.assertEqual(self.query(self.fixture(table([R]*count))),[(-1,0,0,0,0,0)])
  self.assertEqual(self.query(self.fixture(table([R,'broken']))),[(-1,0,0,0,0,0)])
 def test_same_process_target_prefix_release_and_mutation(self):
  text=table([R])+table([R.replace(' 0 2 8 8 0 0',' 1 1 4 4 0 0')],'alternate').replace('XHTEMPLATE 1\n','')
  result=self.query(self.fixture(text),mode='switch');self.assertEqual(result,[(1,0,2,8,8,0)]*3+[(1,1,1,4,4,0),(1,0,2,8,8,0)])
  path=self.fixture(table([R]))
  Path(str(ROOT/path)+'.next').write_text(table([R.replace(' 0 2 8 8 0 0',' 2 1 8 8 0 0')]))
  self.assertEqual(self.query(path,mode='path'),[(1,0,2,8,8,0),(1,2,1,8,8,0),(1,0,2,8,8,0)])
  # ABI-only release retains loaded template strings. Releasing templates
  # advances generation, reloads a mutable same-path file, and refreshes ABI.
  self.assertEqual(self.query(self.fixture(table([R])),mode='mutate'),[(1,0,2,8,8,0),(1,0,2,8,8,0),(1,2,1,8,8,0)])
if __name__=='__main__':unittest.main()
