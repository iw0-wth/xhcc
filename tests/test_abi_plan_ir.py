"""Real IR ABI-plan persistence/merge codec gates; builds only a tiny test harness."""
from pathlib import Path
import os, subprocess, sys, unittest, json, struct, tempfile
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/abi-plan-ir-tests'
HARNESS=r'''
#include "irser.h"
#include "memory.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
ArkccConfig g_config;
void diag_tool(int s,const char*w,const char*m,const char*a,const char*b,const char*c,const char*d,const char*e,const char*f){fprintf(stderr,"%s: %s\n",w,m);}
int ark_strlen(const char*s){return(int)strlen(s);}int ark_streq(const char*a,const char*b){return a&&b&&!strcmp(a,b);}
const char*diag_num(char*b,int n){sprintf(b,"%d",n);return b;}
static unsigned char arena[1024*1024];static unsigned used;
void*mem_pool_alloc(int n){void*p; n=(n+7)&~7;if(n<0||used+(unsigned)n>sizeof(arena))exit(90);p=arena+used;used+=(unsigned)n;memset(p,0,n);return p;}
void*mem_realloc(void*p,int old,int n){void*q=mem_pool_alloc(n);if(p)memcpy(q,p,old<n?old:n);return q;}
char*mem_strdup(const char*s){int n=strlen(s)+1;char*p=mem_pool_alloc(n);memcpy(p,s,n);return p;}
static unsigned char wire[32768],oldwire[32768];
int main(void){IrProgram*p=ir_program_new(),*copy,*dst;IrFunc*f=ir_func_new("caller");IrAbiPlan plan={0};int n,index;
plan.version=1;plan.abi_id=1;plan.rule_fingerprint=123;plan.result.count=1;plan.result.fragments[0].size=8;plan.result.fragments[0].class_id=1;
index=ir_program_add_abi_plan(p,&plan);if(index!=1)return 1;
f->parameter_plan=f->result_plan=index;ir_emit(f,IR_CALL,0,"callee",0);f->code[0].abi_plan=index;ir_program_add(p,f);
n=ir_serialize(p,wire,sizeof(wire),"roundtrip");if(n<0||wire[8]!=2)return 2;
copy=ir_deserialize(wire,n,"roundtrip");if(!copy||copy->abi_plan_count!=1||copy->funcs[0]->code[0].abi_plan!=1||copy->funcs[0]->parameter_plan!=1||copy->funcs[0]->result_plan!=1||copy->abi_plans[0]->rule_fingerprint!=123)return 3;
dst=ir_program_new();if(ir_program_add_abi_plan(dst,&plan)!=1)return 4;ir_program_append_unit(dst,copy,0,"merge");if(dst->abi_plan_count!=2||dst->funcs[0]->code[0].abi_plan!=2||dst->funcs[0]->parameter_plan!=2||dst->funcs[0]->result_plan!=2)return 5;
if(ir_serialize(dst,wire,sizeof(wire),"merge")<0)return 6;
/* Empty v2 has only one added count field: remove it for an actual v1 fixture. */
p=ir_program_new();n=ir_serialize(p,wire,sizeof(wire),"empty");memcpy(oldwire,wire,16);memcpy(oldwire+16,wire+20,n-20);oldwire[8]=1;n-=4;{unsigned body=n-16;int k;for(k=0;k<4;k++)oldwire[12+k]=(unsigned char)(body>>(8*k));}
copy=ir_deserialize(oldwire,n,"legacy-v1");if(!copy||copy->abi_plan_count)return 7;
oldwire[8]=99;if(ir_deserialize(oldwire,n,"unknown-version"))return 8;
puts("IR v2 plans roundtrip merge; v1 absent; unknown rejected");return 0;}
'''
class AbiPlanIr(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  OUT.mkdir(parents=True,exist_ok=True);(OUT/'harness.c').write_text(HARNESS)
  sdk=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))
  env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),env.get('PATH','')])
  args=[str(sdk/'bin/clang.exe'),'--target=wasm32-wasip1','-O2','-ffunction-sections','-fdata-sections','-Iinclude','-Iconfig','-include','include/xh_small.h','build/abi-plan-ir-tests/harness.c','src/ir/ir.c','src/ir/irser.c','src/backend/rules/abiplan_wire.c','-Wl,--gc-sections','-o',str(OUT/'vm.wasm')]
  p=subprocess.run(args,cwd=ROOT,env=env,capture_output=True,text=True)
  if p.returncode:raise RuntimeError(p.stderr)
 def test_real_ir_plan_roundtrip_merge_and_legacy(self):
  runtime=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
  p=subprocess.run([runtime,'run','--dir=.',str(OUT/'vm.wasm')],cwd=ROOT,capture_output=True,text=True)
  self.assertEqual(p.returncode,0,p.stdout+p.stderr)
  self.assertIn('IR v2 plans roundtrip merge',p.stdout)
 def test_frontend_opt_in_generates_real_persistent_plans(self):
  from test_driver import command
  cmd=command()
  with tempfile.TemporaryDirectory(prefix='record-abi-',dir=OUT) as tmp:
   directory=Path(tmp);source=directory/'record.c';obj=directory/'record.o'
   source.write_text('struct Mix { double d; long n; }; extern struct Mix callee(struct Mix); struct Mix entry(struct Mix x) { return callee(x); }\n')
   args=[*cmd,'x86_64-linux-gnu-gcc','-frecord-sysv-abi','-c','--private-object','--obj-ir',str(source.relative_to(ROOT)),'-o',str(obj.relative_to(ROOT))]
   p=subprocess.run(args,cwd=ROOT,capture_output=True,text=True)
   self.assertEqual(p.returncode,0,p.stdout+p.stderr)
   data=obj.read_bytes();offset=data.index(b'ARKIR1\n');self.assertEqual(struct.unpack_from('<I',data,offset+8)[0],2)
   count=struct.unpack_from('<I',data,offset+16)[0];self.assertGreaterEqual(count,2)
   cursor=offset+20
   for _ in range(count):
    length=struct.unpack_from('<I',data,cursor)[0];cursor+=4
    magic,version,abi,fingerprint,argc,stack,sret=struct.unpack_from('<7I',data,cursor)
    self.assertEqual(version,2);self.assertEqual(abi,1);self.assertNotEqual(fingerprint,0);self.assertEqual(argc,1);self.assertGreater(length,60);cursor+=length
if __name__=='__main__':unittest.main()
