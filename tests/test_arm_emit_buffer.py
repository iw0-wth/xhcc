"""Compare actual ARM dynamic/fixed codegen and guard every caller capacity.
Includes the production backend TU only inside the test harness to reach its
private dynamic constructor; no extra production entry point is exported.
"""
from pathlib import Path
import os,sys,subprocess,tempfile,unittest
from test_ir_contract import HARNESS as SUPPORT
ROOT=Path(__file__).resolve().parents[1];SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))/'bin';WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
HARNESS='#include "diag.h"\n'+SUPPORT.split('static unsigned char wire')[0].replace('void diag_tool(int s,','void diag_tool(DiagSeverity s,')+r'''
#include "src/backend/arm/backend_arm.c"
static char reference[131072],fixed[131074];
#define CHECK(x) do{if(!(x)){fprintf(stderr,"check line %d\n",__LINE__);return 1;}}while(0)
int main(void){IrProgram*p=ir_program_new();IrFunc*f=ir_func_new("entry");int mode,k,n,fail;char*dynamic;unsigned mark;CpuInfo cpu={"test",7,0,0,0,0,1,1,0,0};
 arm_set_target_features(&cpu,1);f->ret_type=type_int();f->local_size=8;f->label_count=2;
 ir_emit(f,IR_PUSH_INT,0,0,0);ir_emit(f,IR_STORE,0,0,4);
 for(k=0;k<180;k++){ir_emit(f,IR_PUSH_INT,1,0,0);ir_emit(f,IR_JZ,k+10,0,0);ir_emit(f,IR_LABEL,k+10,0,0);ir_emit(f,IR_PUSH_VAR,0,0,4);ir_emit(f,IR_PUSH_INT,k,0,0);ir_emit(f,IR_BINARY,0,0,TOK_PLUS);ir_emit(f,IR_STORE,0,0,4);}
 ir_emit(f,IR_PUSH_VAR,0,0,4);ir_emit(f,IR_RETURN,0,0,0);ir_program_add(p,f);mark=used;
 for(mode=0;mode<2;mode++){
  g_config.enable_opt_peep=mode;g_config.enable_opt_copyprop=mode;g_config.enable_opt_live=mode;g_config.enable_opt_regalloc=mode;g_config.enable_opt_vstack=mode;g_config.enable_opt_addr=mode;g_config.enable_opt_cbz=mode;g_config.enable_rt_lazy=mode;
  used=mark;xh_arm_rules_reset();dynamic=codegen_arm(p,0,0,&fail);CHECK(!fail);n=(int)strlen(dynamic);CHECK(n>4096&&n<131072);memcpy(reference,dynamic,n+1);
  used=mark;memset(fixed,0x5a,sizeof fixed);xh_arm_rules_reset();CHECK(codegen_arm(p,fixed+1,131072,&fail)==fixed+1);CHECK(!fail&&!strcmp(reference,fixed+1));CHECK(fixed[0]==0x5a&&fixed[131073]==0x5a);
  used=mark;memset(fixed,0x5a,sizeof fixed);{int capacity=mode?131072:n+1;int got=arm_emit(p,fixed+1,capacity);if(got!=n)fprintf(stderr,"mode %d got %d expected %d\n",mode,got,n);CHECK(got==n);CHECK(!memcmp(reference,fixed+1,n+1));CHECK(fixed[0]==0x5a&&fixed[capacity+1]==0x5a);}
  for(k=0;k<4;k++){int cap=k==0?0:k==1?1:k==2?100:n;used=mark;memset(fixed,0x5a,sizeof fixed);CHECK(arm_emit(p,fixed+1,cap)<0);CHECK(fixed[0]==0x5a&&fixed[cap+1]==0x5a);if(cap)CHECK(fixed[1]==0);}
 }
 CHECK(arm_emit(p,0,100)<0);puts("PASS ARM dynamic/fixed exact text, optimizers, capacities and canaries");return 0;}
'''
class ArmEmitBuffer(unittest.TestCase):
 def test_private_dynamic_reference_and_fixed_production(self):
  with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-emit-buffer-') as tmp:
   out=Path(tmp);src=out/'h.c';src.write_text(HARNESS);env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),env.get('PATH','')]);vm=out/'h.wasm'
   sources=['src/ir/ir.c','src/ir/stack_depth.c','src/ir/ir_contract.c','src/ir/ir_contract_legacy.c','src/frontend/c/type.c','src/frontend/c/agginit.c','src/core/codegen_common.c','src/backend/arm/thumb1.c','src/backend/rules/arm_rules.c','src/backend/rules/template.c','src/backend/rules/semantic.c','src/backend/rules/abi.c','src/backend/rules/encoder.c','src/backend/rules/relocation.c','src/backend/rules/selector.c','src/backend/rules/abiplan_wire.c','src/core/scatter.c']
   p=subprocess.run([str(SDK/'clang.exe'),'--target=wasm32-wasip1','-O2','-ffunction-sections','-fdata-sections','-I.','-Iinclude','-Iconfig','-Iobject','-include','include/xh_small.h','-DTEST_ARM','-DXH_IR_TEST_DIR="build"',str(src),*sources,'-Wl,--gc-sections','-o',str(vm)],cwd=ROOT,env=env,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
   p=subprocess.run([WASM,'run','--dir=.',str(vm)],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);self.assertIn('PASS',p.stdout)
if __name__=='__main__':unittest.main()
