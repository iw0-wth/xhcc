"""Actual ARM assembler capacity preflight and sticky-write canary tests."""
from pathlib import Path
import os,sys,subprocess,tempfile,unittest
from test_ir_contract import HARNESS as SUPPORT
ROOT=Path(__file__).resolve().parents[1];SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))/'bin';WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
base=SUPPORT.split('static unsigned char wire')[0]
base=base.replace('void diag_tool(int s,','void diag_tool(DiagSeverity s,').replace('fprintf(stderr,"%s: %s\\n",w,m);','diagnostics++;fprintf(stderr,"%s: %s\\n",w,m);')
HARNESS='#include "diag.h"\nstatic int diagnostics;\n'+base+r'''
#include "src/backend/arm/backend_arm.c"
static unsigned char output[8194],reference[8192];
#define CHECK(x) do{if(!(x)){fprintf(stderr,"failed %d\n",__LINE__);return 1;}}while(0)
static int check(const char*text,int expected,int shrink){int rc,k;g_config.enable_opt_branch=shrink;
 memset(output,0xa5,sizeof output);diagnostics=0;rc=arm_assemble(text,output+1,8192);CHECK(rc==expected);CHECK(!diagnostics);memcpy(reference,output+1,expected);
 memset(output,0xa5,sizeof output);diagnostics=0;rc=arm_assemble(text,output+1,expected);CHECK(rc==expected);CHECK(!memcmp(reference,output+1,expected));CHECK(output[0]==0xa5&&output[expected+1]==0xa5);
 for(k=0;k<3;k++){int cap=k==0?0:k==1?1:expected-1;memset(output,0xa5,sizeof output);diagnostics=0;CHECK(arm_assemble(text,output+1,cap)<0);CHECK(diagnostics==1);CHECK(output[0]==0xa5&&output[cap+1]==0xa5);CHECK(arm_get_symbol_count()==0&&arm_get_reloc_count()==0);}
 return 0;}
int main(void){CpuInfo cpu={"test",7,0,0,0,0,1,1,0,0};ArmAsm a;int before;arm_set_target_features(&cpu,1);
 CHECK(check(".syntax unified\n.thumb\nentry:\nMOV r0,#17\nBX lr\n",4,0)==0);
 CHECK(check(".word 1,2\n.byte 3,4,5\n.space 7\n.align 4\n.ascii \"abc\"\n.asciz \"hi\"\n",28,0)==0);
 CHECK(check("entry:\nB .Lend\nNOP\n.Lend:\nBX lr\n",8,0)==0);
 CHECK(check("entry:\nB .Lend\nNOP\n.Lend:\nBX lr\n",6,1)==0);
 g_arm_vt_reserve=1;CHECK(check("entry:\nBX lr\n",ARM_VT_SIZE+2,0)==0);g_arm_vt_reserve=0;
 CHECK(arm_assemble("",0,0)==0);CHECK(arm_assemble("NOP\n",0,1)<0);
 diagnostics=0;CHECK(arm_assemble(".space 9999999999999999999999\n",output+1,10)<0);CHECK(diagnostics==1);
 diagnostics=0;CHECK(arm_assemble(".org 100\nNOP\n",output+1,10)<0);CHECK(diagnostics==1);
 /* Directly exercise the secondary guard if emission exceeds a stale plan. */
 memset(&a,0,sizeof a);memset(output,0xa5,sizeof output);a.out=output+1;a.out_cap=1;out16(&a,0x1234);CHECK(a.overflow&&a.pc==0);before=a.hard_errors;out8(&a,9);out32(&a,9);CHECK(a.hard_errors==before&&output[0]==0xa5&&output[1]==0xa5&&output[2]==0xa5);
 memset(&a,0,sizeof a);a.src=".byte 1,2\n.word 9\n";a.out=output+1;a.out_cap=1;pass2_a(&a);CHECK(a.overflow&&a.pc==1&&output[2]==0xa5);before=a.ref_count;resolve_refs_a(&a);CHECK(a.ref_count==before);
 puts("PASS ARM capacity exact bytes, directives, relaxed branches, vector prefix and sticky overflow");return 0;}
'''
class ArmOutputCapacity(unittest.TestCase):
 def test_actual_assembler_capacity(self):
  with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-output-capacity-') as tmp:
   out=Path(tmp);src=out/'h.c';src.write_text(HARNESS);env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),env.get('PATH','')]);vm=out/'h.wasm'
   sources=['src/ir/ir.c','src/ir/stack_depth.c','src/ir/ir_contract.c','src/ir/ir_contract_legacy.c','src/frontend/c/type.c','src/frontend/c/agginit.c','src/core/codegen_common.c','src/backend/arm/thumb1.c','src/backend/rules/arm_rules.c','src/backend/rules/template.c','src/backend/rules/semantic.c','src/backend/rules/abi.c','src/backend/rules/encoder.c','src/backend/rules/relocation.c','src/backend/rules/selector.c','src/backend/rules/abiplan_wire.c','src/core/scatter.c']
   p=subprocess.run([str(SDK/'clang.exe'),'--target=wasm32-wasip1','-O2','-ffunction-sections','-fdata-sections','-I.','-Iinclude','-Iobject','-include','include/xh_small.h','-DTEST_ARM','-DXH_IR_TEST_DIR="build"',str(src),*sources,'-Wl,--gc-sections','-o',str(vm)],cwd=ROOT,env=env,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
   p=subprocess.run([WASM,'run','--dir=.',str(vm)],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);self.assertIn('PASS',p.stdout)
if __name__=='__main__':unittest.main()
