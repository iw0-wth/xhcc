"""Naked basic asm owns entry registers; parameter homing must not touch caller FP."""
from pathlib import Path
import sys,os,subprocess,tempfile,unittest
from test_driver import ROOT,command
sys.path.insert(0,str(ROOT/'build/test-deps'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_SP,UC_ARM_REG_LR
GNU=Path(os.environ.get('GNUARM',r'D:\arm-none-eabi'))/'bin'
class ArmNakedParameters(unittest.TestCase):
 def test_basic_asm_named_parameter_preserves_caller_locals(self):
  with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-naked-') as tmp:
   out=Path(tmp);src=out/'a.c';obj=out/'a.o';exe=out/'a.elf'
   src.write_text('__attribute__((naked)) unsigned leaf(void*p){asm volatile("movs r0, #7\\nbx lr");return 0;}\nint entry(void){int x=31;unsigned y=leaf(&x);return x+y;}\n')
   p=subprocess.run([*command(),'arm-none-eabi-gcc','-mcpu=cortex-m3','-O0','-c',str(src.relative_to(ROOT)),'-o',str(obj.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
   p=subprocess.run([str(GNU/'arm-none-eabi-ld.exe'),'-Ttext=0x10000','-e','entry','-o',str(exe),str(obj)],capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
   with exe.open('rb') as f:
    e=ELFFile(f);sym=e.get_section_by_name('.symtab').get_symbol_by_name('leaf')[0];sec=e.get_section(sym['st_shndx']);start=(sym['st_value']&~1)-sec['sh_addr'];self.assertEqual(sec.data()[start:start+2],bytes.fromhex('0720'))
    uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS);uc.mem_map(0x10000,0x20000);uc.mem_map(0x700000,0x20000)
    for s in e.iter_sections():
     if s['sh_flags']&2 and s['sh_type']!='SHT_NOBITS':uc.mem_write(s['sh_addr'],s.data())
    uc.reg_write(UC_ARM_REG_SP,0x71f000);uc.reg_write(UC_ARM_REG_LR,0x2f001);uc.emu_start(e['e_entry']|1,0x2f000,count=10000);self.assertEqual(uc.reg_read(UC_ARM_REG_R0),38);self.assertEqual(uc.reg_read(UC_ARM_REG_SP),0x71f000)
 def test_frame_dependent_naked_c_is_rejected(self):
  with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-naked-bad-') as tmp:
   out=Path(tmp)
   for i,body in enumerate(['return x;','int local=3;return local;']):
    src=out/(str(i)+'.c');obj=src.with_suffix('.o');src.write_text('__attribute__((naked)) int bad(int x){'+body+'}\n')
    p=subprocess.run([*command(),'arm-none-eabi-gcc','-mcpu=cortex-m3','-O0','-c',str(src.relative_to(ROOT)),'-o',str(obj.relative_to(ROOT))],cwd=ROOT,capture_output=True,text=True);self.assertNotEqual(p.returncode,0,p.stdout+p.stderr);self.assertIn('frame access',p.stderr);self.assertFalse(obj.exists())
if __name__=='__main__':unittest.main()
