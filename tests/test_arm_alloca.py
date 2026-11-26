"""Actual ARM Thumb-2 alloca live-stack preservation and frame restoration.
Run after unified production build; this test never rebuilds the compiler.
"""
from pathlib import Path
import os,sys,subprocess,tempfile,unittest
from test_driver import ROOT, command
sys.path.insert(0,str(ROOT/'build/test-deps'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_SP,UC_ARM_REG_LR
GNU=Path(os.environ.get('GNUARM',r'D:\arm-none-eabi'))/'bin'
SOURCE=r'''
#include <alloca.h>
int sink(void *p,int n){unsigned char *q=p;q[0]=9;q[8]=11;return n+q[0]+q[8];}
int arg_live(void){return sink(alloca(9),17);}
int lhs_live(void){void *p[1];p[0]=alloca(9);((char*)p[0])[0]=31;return ((char*)p[0])[0];}
int multi_live(void){return 100+sink(alloca(9),17);}
int repeated(void){unsigned char *a=alloca(9),*b=alloca(17);a[0]=3;a[8]=4;b[0]=5;b[16]=6;return a[0]+a[8]+b[0]+b[16];}
int conditional(int k){unsigned char*p=k?alloca(9):alloca(17);p[0]=13;return p[0];}
int branches(void){return conditional(0)+conditional(1);}
int zero_size(void){void*p=alloca(0);return ((unsigned)p&7)==0;}
int aligned(void){void*p=alloca(9);return ((unsigned)p&7)==0;}
int dynamic(int n){unsigned char*p=alloca(n);p[0]=7;p[n-1]=8;return p[0]+p[n-1];}
int varied(void){return dynamic(9)+dynamic(17);}
int loop_alloca(void){int i,sum=0;for(i=0;i<9;i++){unsigned char*p=alloca(i+1);p[0]=i;sum=sum+p[0];}return sum;}
void noresult(void){char*p=alloca(5);p[0]=1;}
int void_call(void){noresult();return 19;}
'''
class ArmAlloca(unittest.TestCase):
 def run_cmd(self,args,ok=True):
  p=subprocess.run(list(map(str,args)),cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode==0,ok,p.stdout+p.stderr);return p
 def test_live_values_control_and_stack_restore(self):
  with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-alloca-') as tmp:
   out=Path(tmp);src=out/'a.c';src.write_text(SOURCE)
   for opt in ['-O0','-O2']:
    obj=out/(opt+'.o');exe=out/(opt+'.elf')
    self.run_cmd([*command(),'arm-none-eabi-gcc','-mcpu=cortex-m3',opt,'-fno-builtin','-c',src.relative_to(ROOT),'-o',obj.relative_to(ROOT)])
    self.run_cmd([GNU/'arm-none-eabi-ld.exe','-Ttext=0x10000','-e','arg_live','-o',exe,obj])
    with exe.open('rb') as f:
     elf=ELFFile(f);symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()};uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS);uc.mem_map(0x10000,0x40000);uc.mem_map(0x700000,0x20000)
     for sec in elf.iter_sections():
      if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
     for name,want in [('arg_live',37),('lhs_live',31),('multi_live',137),('repeated',18),('branches',26),('zero_size',1),('aligned',1),('varied',30),('loop_alloca',36),('void_call',19)]:
      for k in range(30):
       sp=0x71f000;uc.reg_write(UC_ARM_REG_SP,sp);uc.reg_write(UC_ARM_REG_LR,0x4f001);uc.emu_start(symbols[name]|1,0x4f000,count=100000)
       self.assertEqual(uc.reg_read(UC_ARM_REG_R0),want,(opt,name,k));self.assertEqual(uc.reg_read(UC_ARM_REG_SP),sp,(opt,name,'stack restore'))
 def test_thumb1_and_unverified_asm_remain_rejected(self):
  with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='arm-alloca-bad-') as tmp:
   out=Path(tmp);src=out/'a.c';src.write_text('#include <alloca.h>\nint entry(void){char*p=alloca(4);return p!=0;}\n')
   obj=out/'a.o';self.run_cmd([*command(),'arm-none-eabi-gcc','-mcpu=cortex-m0','-c',src.relative_to(ROOT),'-o',obj.relative_to(ROOT)],ok=False);self.assertFalse(obj.exists())
   src.write_text('#include <alloca.h>\nint entry(void){char*p=alloca(4);asm volatile("nop");return p!=0;}\n')
   self.run_cmd([*command(),'arm-none-eabi-gcc','-mcpu=cortex-m3','-c',src.relative_to(ROOT),'-o',obj.relative_to(ROOT)],ok=False);self.assertFalse(obj.exists())
if __name__=='__main__':unittest.main()
