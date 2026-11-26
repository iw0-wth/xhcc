"""GNU ARM no-output scalar/pointer bindings retain side effects and no result."""
from pathlib import Path
import os,sys,tempfile,subprocess,unittest
from test_driver import ROOT,command
sys.path.insert(0,str(ROOT/'build/test-deps'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_R0
GNU=Path(os.environ.get('GNUARM',r'D:\arm-none-eabi'))/'bin'
class GnuAsmInputs(unittest.TestCase):
 def test_pointer_input_without_output_executes(self):
  with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='asm-input-') as tmp:
   out=Path(tmp);src=out/'one.c';obj=out/'one.o';exe=out/'one.elf'
   src.write_text('int entry(void){int x=0;int*p=&x;int value=37;asm volatile("str %1, [%0]" :: "r"(p),"r"(value):"memory");return x+5;}\n')
   p=subprocess.run([*command(),'arm-none-eabi-gcc','-mcpu=cortex-m3','-O0','-c',str(src.relative_to(ROOT)),'-o',str(obj.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
   p=subprocess.run([str(GNU/'arm-none-eabi-ld.exe'),'-Ttext=0x10000','-e','entry','-o',str(exe),str(obj)],capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
   with exe.open('rb') as f:
    elf=ELFFile(f);entry=elf['e_entry'];uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS);uc.mem_map(0x10000,0x40000);uc.mem_map(0x700000,0x20000)
    for sec in elf.iter_sections():
     if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
    uc.reg_write(UC_ARM_REG_SP,0x71f000);uc.reg_write(UC_ARM_REG_LR,0x4f001);uc.emu_start(entry|1,0x4f000,count=100000);self.assertEqual(uc.reg_read(UC_ARM_REG_R0),42);self.assertEqual(uc.reg_read(UC_ARM_REG_SP),0x71f000)
if __name__=='__main__':unittest.main()
