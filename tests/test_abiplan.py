"""Standalone transactional ABI allocator; no production aggregate lowering.
Self-designed location records do not establish native compiler interoperability.
"""
from pathlib import Path
import os,sys,subprocess,unittest
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/abiplan-tests'
SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'));WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
HARNESS=r'''
#include "xh_abiplan.h"
#include "xh_template.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
int fs_file_size(const char*p){FILE*f=fopen(p,"rb");long n;if(!f)return -1;fseek(f,0,SEEK_END);n=ftell(f);fclose(f);return(int)n;}
int fs_read_file(const char*p,char*b,int cap){FILE*f=fopen(p,"rb");int n;if(!f)return -1;n=(int)fread(b,1,cap-1,f);b[n]=0;fclose(f);return n;}
void diag_tool(int s,const char*w,const char*m,const char*a,const char*b,const char*c,const char*d,const char*e,const char*f){fprintf(stderr,"%s: %s\n",w,m);}
static XhTypeclassResult classify(unsigned n,int a,int b){XhTypeclassResult c;memset(&c,0,sizeof(c));c.count=n;c.fragments[0].size=8;c.fragments[0].class_id=a;c.fragments[1].offset=8;c.fragments[1].size=8;c.fragments[1].class_id=b;return c;}
static int place(XhAbiPlanPolicy*p,XhAbiPlanState*s,XhTypeclassResult*c,unsigned size,unsigned align){XhAbiArgumentPlan out,before;XhAbiPlanState old=*s;unsigned i;int r;memset(&out,0xa5,sizeof(out));before=out;r=xh_abiplan_argument(p,s,c,size,align,&out);printf("arg %d %u %u %u",r,s->used[0],s->used[1],s->stack_size);if(r){printf(" atomic=%d",!memcmp(&old,s,sizeof(old))&&!memcmp(&before,&out,sizeof(out)));}else{printf(" stack=%u offset=%u extent=%u count=%u",out.by_value_stack,out.stack_offset,out.stack_extent,out.count);for(i=0;i<out.count;i++)printf(" loc=%u,%u,%u,%u",out.fragments[i].bank,out.fragments[i].index,out.fragments[i].object_offset,out.fragments[i].size);}puts("");return r;}
int main(int argc,char**argv){XhAbiPlanPolicy p;XhAbiPlanState s;XhTypeclassResult c;unsigned i,bytes;int r;if(argc!=3)return 2;if(xh_typeclass_set_path("x86_64",argv[1]))return 2;memset(&p,0xa5,sizeof(p));r=xh_abiplan_load("x86_64.aggregate","abiplan",&p);if(r){puts("badpolicy");return 0;}if(xh_abiplan_begin(&p,!strcmp(argv[2],"sret"),&s))return 2;
if(!strcmp(argv[2],"gp")){c=classify(1,1,0);for(i=0;i<6;i++)place(&p,&s,&c,8,8);c=classify(2,2,1);place(&p,&s,&c,16,8);c=classify(1,2,0);place(&p,&s,&c,8,8);}
else if(!strcmp(argv[2],"fp")){c=classify(1,2,0);for(i=0;i<8;i++)place(&p,&s,&c,8,8);c=classify(2,1,2);place(&p,&s,&c,16,8);c=classify(1,1,0);place(&p,&s,&c,8,8);}
else if(!strcmp(argv[2],"sret")){c=classify(2,1,2);place(&p,&s,&c,16,8);}
else if(!strcmp(argv[2],"memory")){memset(&c,0,sizeof(c));c.memory=1;place(&p,&s,&c,24,8);place(&p,&s,&c,9,16);}
else if(!strcmp(argv[2],"share")){c=classify(2,2,3);place(&p,&s,&c,16,8);c=classify(2,0,1);place(&p,&s,&c,16,8);c=classify(1,3,0);place(&p,&s,&c,8,8);}
else if(!strcmp(argv[2],"bad")){c=classify(1,99,0);place(&p,&s,&c,8,8);c=classify(1,1,0);place(&p,&s,&c,4,8);place(&p,&s,&c,8,3);s.stack_size=UINT_MAX-3;memset(&c,0,sizeof(c));c.memory=1;place(&p,&s,&c,24,8);s.stack_size=0;s.argument_count=64;c=classify(1,1,0);place(&p,&s,&c,8,8);}
else if(!strcmp(argv[2],"graph")){XhTypeclassNode nodes[3]={{0,0,16,8,0,2},{1,3,8,8,0,0},{1,1,8,8,0,0}};XhTypeclassEdge edges[2]={{1,0},{2,8}};XhTypeclassGraph g={nodes,3,edges,2};if(xh_typeclass_select("x86_64.aggregate","typeclass",&g,0,&c))return 2;place(&p,&s,&c,16,8);}
else return 2;
if(xh_abiplan_finish(&p,&s,&bytes))return 2;printf("finish %u\n",bytes);return 0;}
'''
class AbiPlan(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  OUT.mkdir(parents=True,exist_ok=True);(OUT/'harness.c').write_text(HARNESS);env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),env.get('PATH','')])
  p=subprocess.run([str(SDK/'bin/clang.exe'),'--target=wasm32-wasip1','-O2','-Iinclude','-include','include/xh_small.h','build/abiplan-tests/harness.c','src/backend/rules/abiplan.c','src/backend/rules/typeclass.c','src/backend/rules/template.c','-o','build/abiplan-tests/vm.wasm'],cwd=ROOT,env=env,capture_output=True,text=True)
  if p.returncode:raise RuntimeError(p.stderr)
 def run_case(self,case,path='packages/x86_64/aggregate.rules'):
  p=subprocess.run([WASM,'run','--dir=.','build/abiplan-tests/vm.wasm',str(path),case],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);return p.stdout.splitlines()
 def test_register_budget_rollback_both_banks(self):
  lines=self.run_case('gp');self.assertIn('arg 0 6 0 16 stack=1 offset=0 extent=16 count=0',lines);self.assertIn('arg 0 6 1 16 stack=0 offset=0 extent=0 count=1 loc=1,0,0,8',lines);self.assertEqual(lines[-1],'finish 16')
  lines=self.run_case('fp');self.assertIn('arg 0 0 8 16 stack=1 offset=0 extent=16 count=0',lines);self.assertIn('arg 0 1 8 16 stack=0 offset=0 extent=0 count=1 loc=0,0,0,8',lines)
 def test_sret_memory_stack_alignment_and_real_classification(self):
  self.assertEqual(self.run_case('sret')[0],'arg 0 2 1 0 stack=0 offset=0 extent=0 count=2 loc=0,1,0,8 loc=1,0,8,8')
  lines=self.run_case('memory');self.assertEqual(lines,['arg 0 0 0 24 stack=1 offset=0 extent=24 count=0','arg 0 0 0 48 stack=1 offset=32 extent=16 count=0','finish 48'])
  self.assertEqual(self.run_case('graph')[0],'arg 0 1 1 0 stack=0 offset=0 extent=0 count=2 loc=1,0,0,8 loc=0,0,8,8')
 def test_share_previous_padding_and_failure_atomicity(self):
  lines=self.run_case('share');self.assertIn('arg 0 0 1 0 stack=0 offset=0 extent=0 count=2 loc=1,0,0,8 loc=1,0,8,8',lines);self.assertIn('arg 0 1 1 0 stack=0 offset=0 extent=0 count=1 loc=0,0,8,8',lines);self.assertIn('atomic=1',lines[2])
  lines=self.run_case('bad');self.assertEqual(len(lines),6);self.assertTrue(all('atomic=1' in line for line in lines[:-1]))
 def test_policy_change_and_rejection(self):
  base=(ROOT/'packages/x86_64/aggregate.rules').read_text();path=OUT/'variant.rules';path.write_text(base.replace('text 1 6 8 8 16 16 0 1 5','text 1 0 8 8 16 16 0 0 5'));lines=self.run_case('sret',path.relative_to(ROOT));self.assertIn('stack=1',lines[0])
  for text in [base.replace('text 1 6 8 8 16 16 0 1 5','text 1 6 8 3 16 16 0 1 5'),base.replace('text 1 0 1\n','text 1 1 1\n'),base.replace('rule abiplan.class.1\n','rule missing.abiplan.class.1\n')]:
   path.write_text(text);self.assertEqual(self.run_case('sret',path.relative_to(ROOT)),['badpolicy'])
if __name__=='__main__':unittest.main()
