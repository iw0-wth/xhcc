"""Late ARM externs remain symbolic beyond optional cache; real GNU/CPU oracle."""
from pathlib import Path
import sys,os,subprocess,tempfile,unittest
from test_driver import ROOT,command
sys.path.insert(0,str(ROOT/'build/test-deps'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_SP,UC_ARM_REG_LR
GNU=Path(os.environ.get('GNUARM',r'D:\arm-none-eabi'))/'bin'
class ArmExternCapacity(unittest.TestCase):
 def run_cmd(self,args):
  p=subprocess.run(list(map(str,args)),cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr);return p
 def test_late_addresses_and_reads_link_and_execute(self):
  with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-extern-cap-') as tmp:
   out=Path(tmp);src=out/'a.c';defs=out/'defs.c';defs.write_text('unsigned first=7,late=35; unsigned long long wide=4294967301ULL;\n')
   ref=out/'defs.o';self.run_cmd([GNU/'arm-none-eabi-gcc.exe','-mcpu=cortex-m3','-mthumb','-ffreestanding','-c',defs,'-o',ref])
   for count in [129,257]:
    src.write_text('extern unsigned first;\n'+''.join('extern unsigned unused_'+str(i)+';\n' for i in range(count))+'extern unsigned late;extern unsigned long long wide;\nunsigned* address(void){return &late;}\nunsigned entry(void){return first+*address()+(unsigned)(wide>>32)+(unsigned)wide;}\n')
    obj=out/(str(count)+'.o');exe=out/(str(count)+'.elf');self.run_cmd([*command(),'arm-none-eabi-gcc','-mcpu=cortex-m3','-O0','-c',src.relative_to(ROOT),'-o',obj.relative_to(ROOT)])
    with obj.open('rb') as f:
     e=ELFFile(f);names={s.name for s in e.get_section_by_name('.symtab').iter_symbols()};self.assertTrue({'first','late','wide'}<=names)
    lib=self.run_cmd([GNU/'arm-none-eabi-gcc.exe','-mcpu=cortex-m3','-mthumb','-print-libgcc-file-name']).stdout.strip()
    self.run_cmd([GNU/'arm-none-eabi-ld.exe','-Ttext=0x10000','-Tdata=0x20000000','-e','entry','-o',exe,obj,ref,lib])
    with exe.open('rb') as f:
     e=ELFFile(f);uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS);uc.mem_map(0x10000,0x40000);uc.mem_map(0x20000000,0x20000)
     for sec in e.iter_sections():
      if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
     uc.reg_write(UC_ARM_REG_SP,0x2001f000);uc.reg_write(UC_ARM_REG_LR,0x4f001);uc.emu_start(e['e_entry']|1,0x4f000,count=100000);self.assertEqual(uc.reg_read(UC_ARM_REG_R0),48)
 def test_same_process_reset_and_unknown_ir_rejection(self):
  from test_arm_emit_buffer import HARNESS as ORIGINAL
  start=ORIGINAL.index('int main(void)')
  harness=ORIGINAL[:start]+r'''
int main(void){IrProgram*p=ir_program_new(),*next;IrFunc*f=ir_func_new("entry");char name[64];int i,n;CpuInfo cpu={"test",7,0,0,0,0,1,1,0,0};arm_set_target_features(&cpu,1);g_xhcc_elf_mode=1;
for(i=0;i<270;i++){IrGlobal*g;snprintf(name,sizeof name,"ext%d",i);g=ir_program_add_global(p,mem_strdup(name),type_int());g->is_extern=1;}
f->ret_type=type_int();ir_emit(f,IR_PUSH_GLOBAL_ADDR,0,"ext269",0);ir_emit(f,IR_RETURN,0,0,0);ir_program_add(p,f);n=arm_emit(p,fixed,sizeof fixed);CHECK(n>0&&strstr(fixed,"ext269"));CHECK(g_gvar_program==0);
next=ir_program_new();f=ir_func_new("next");f->ret_type=type_int();ir_emit(f,IR_PUSH_GLOBAL_ADDR,0,"ext269",0);ir_emit(f,IR_RETURN,0,0,0);ir_program_add(next,f);CHECK(arm_emit(next,fixed,sizeof fixed)<0&&fixed[0]==0);CHECK(g_gvar_program==0);
puts("PASS full extern declarations, TU reset and unknown IR rejection");return 0;}
'''
  with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-extern-ir-') as tmp:
   out=Path(tmp);src=out/'h.c';src.write_text(harness);vm=out/'h.wasm';sdk=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'))/'bin';env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),env.get('PATH','')])
   sources=['src/ir/ir.c','src/ir/stack_depth.c','src/ir/ir_contract.c','src/ir/ir_contract_legacy.c','src/frontend/c/type.c','src/frontend/c/agginit.c','src/core/codegen_common.c','src/backend/arm/thumb1.c','src/backend/rules/arm_rules.c','src/backend/rules/template.c','src/backend/rules/semantic.c','src/backend/rules/abi.c','src/backend/rules/encoder.c','src/backend/rules/relocation.c','src/backend/rules/selector.c','src/backend/rules/abiplan_wire.c','src/core/scatter.c']
   p=subprocess.run([str(sdk/'clang.exe'),'--target=wasm32-wasip1','-O2','-ffunction-sections','-fdata-sections','-I.','-Iinclude','-Iobject','-include','include/xh_small.h','-DTEST_ARM','-DXHCC','-DXH_IR_TEST_DIR="build"',str(src),*sources,'-Wl,--gc-sections','-o',str(vm)],cwd=ROOT,env=env,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
   p=subprocess.run([r'C:\Program Files\Wasmtime\bin\wasmtime.exe','run','--dir=.',str(vm)],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);self.assertIn('PASS',p.stdout)
if __name__=='__main__':unittest.main()
