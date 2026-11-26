"""ARM string decoding has one layout/emission contract; GNU is byte oracle."""
from pathlib import Path
import tempfile,subprocess,os,sys,unittest
from test_arm_output_capacity import HARNESS as CAP_SUPPORT
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'build/test-deps'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_LR,UC_ARM_REG_SP
GNU=Path(os.environ.get('GNUARM',r'D:\arm-none-eabi'))/'bin';SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))/'bin';WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
FIXTURE=r'''.syntax unified
.thumb
.text
.global hex_first
hex_first:
.asciz "\x04"
.global hex_after
hex_after:
nop
.global mixed_first
mixed_first:
.ascii "A\x42\n\t\r\\\"\0"
.global mixed_after
mixed_after:
bx lr
.global entry
.thumb_func
entry:
b.w .Lcode
.asciz "\x01"
.asciz "\x02"
.asciz "\x03"
.balign 4
.Lcode:
movs r0,#42
bx lr
'''
HARNESS=CAP_SUPPORT.split('static unsigned char output')[0]+r'''
static unsigned char output[4098];static char text[4096];
#define CHECK(x) do{if(!(x)){fprintf(stderr,"failed %d\n",__LINE__);return 1;}}while(0)
int main(int argc,char**argv){FILE*f;int n,rc,i;char path[512];if(argc!=2)return 2;snprintf(path,sizeof path,"%s/input.s",argv[1]);f=fopen(path,"rb");CHECK(f);n=(int)fread(text,1,sizeof(text)-1,f);fclose(f);text[n]=0;arm_set_target_features(0,0);g_config.enable_opt_branch=0;
 memset(output,0xa5,sizeof output);rc=arm_assemble(text,output+1,4096);CHECK(rc>0);CHECK(output[0]==0xa5&&output[4097]==0xa5);snprintf(path,sizeof path,"%s/output.bin",argv[1]);f=fopen(path,"wb");CHECK(f);fwrite(output+1,1,rc,f);fclose(f);
 for(i=0;i<g_arm_sym_count;i++)printf("%s %d\n",g_arm_syms[i].name,g_arm_syms[i].offset);
 memset(output,0xa5,sizeof output);CHECK(arm_assemble(text,output+1,rc)==rc);CHECK(output[0]==0xa5&&output[rc+1]==0xa5);memset(output,0xa5,sizeof output);CHECK(arm_assemble(text,output+1,rc-1)<0);CHECK(output[1]==0xa5&&output[rc]==0xa5);
 memset(output,0xa5,sizeof output);CHECK(arm_assemble(".asciz \"abc\\",output+1,4096)<0);CHECK(!arm_get_symbol_count()&&!arm_get_reloc_count());CHECK(output[1]==0xa5);
 CHECK(arm_assemble(".ascii \"abc\n",output+1,4096)<0);CHECK(output[1]==0xa5);
 CHECK(arm_assemble(".asciz \"\\x04\"\nNOP\n",output+1,4)==4);
 return 0;}
'''
class ArmStringLayout(unittest.TestCase):
 def test_layout_bytes_symbols_and_cpu_match_gnu(self):
  with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-string-layout-') as tmp:
   out=Path(tmp);(out/'input.s').write_text(FIXTURE);src=out/'h.c';src.write_text(HARNESS);vm=out/'h.wasm';env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),env.get('PATH','')])
   sources=['src/ir/ir.c','src/ir/stack_depth.c','src/ir/ir_contract.c','src/ir/ir_contract_legacy.c','src/frontend/c/type.c','src/frontend/c/agginit.c','src/core/codegen_common.c','src/backend/arm/thumb1.c','src/backend/rules/arm_rules.c','src/backend/rules/template.c','src/backend/rules/semantic.c','src/backend/rules/abi.c','src/backend/rules/encoder.c','src/backend/rules/relocation.c','src/backend/rules/selector.c','src/backend/rules/abiplan_wire.c','src/core/scatter.c']
   def run(args):
    p=subprocess.run(list(map(str,args)),cwd=ROOT,env=env,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr);return p
   run([SDK/'clang.exe','--target=wasm32-wasip1','-O2','-ffunction-sections','-fdata-sections','-I.','-Iinclude','-Iobject','-include','include/xh_small.h','-DTEST_ARM','-DXH_IR_TEST_DIR="build"',src,*sources,'-Wl,--gc-sections','-o',vm])
   actual=run([WASM,'run','--dir=.',vm,out.relative_to(ROOT)]);syms={line.split()[0]:int(line.split()[1]) for line in actual.stdout.splitlines()};self.assertEqual([syms[x] for x in ['hex_first','hex_after','mixed_first','mixed_after','entry']],[0,2,4,12,14])
   obj=out/'gnu.o';run([GNU/'arm-none-eabi-as.exe','-mcpu=cortex-m4','-mthumb',out/'input.s','-o',obj])
   with obj.open('rb') as f:
    elf=ELFFile(f);expected=elf.get_section_by_name('.text').data();gnu={s.name:s['st_value']&~1 for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name in syms}
   data=(out/'output.bin').read_bytes();self.assertEqual(data,expected);self.assertEqual({k:syms[k] for k in gnu},gnu)
   uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS);uc.mem_map(0x10000,0x20000);uc.mem_map(0x700000,0x20000);uc.mem_write(0x10000,data);uc.reg_write(UC_ARM_REG_SP,0x71f000);uc.reg_write(UC_ARM_REG_LR,0x2f001);uc.emu_start((0x10000+syms['entry'])|1,0x2f000,count=1000);self.assertEqual(uc.reg_read(UC_ARM_REG_R0),42)
if __name__=='__main__':unittest.main()
