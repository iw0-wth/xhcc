"""Compile and execute Thumb-2 ELF for M3/M4/M7/M33 with CPU/FPU macros."""
from pathlib import Path
import os,subprocess,sys
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'build/test-deps'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_R0,UC_ARM_REG_XPSR
WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe');ARM=Path(r'D:\arm-none-eabi\bin');OUT=ROOT/'build/cpu-tests';OUT.mkdir(exist_ok=True,parents=True)
def run(args):
 args=list(map(str,args))
 if args and args[0]==WASM:
  index=next(i for i,v in enumerate(args) if v.endswith('.wasm'))
  rest=args[index+1:]
  if not rest or rest[0].startswith('-'):rest.insert(0,'arm-none-eabi-gcc')
  import shlex
  command=os.environ.get('XHCC_COMMAND')
  args=(shlex.split(command,posix=False) if command else [sys.executable,str(ROOT/'tools/xhcc.py')])+rest
 p=subprocess.run(list(map(str,args)),cwd=ROOT,capture_output=True,text=True)
 if p.returncode:raise RuntimeError(f'{args}\n{p.stdout}\n{p.stderr}')
for cpu,arch in [('cortex-m3',7),('cortex-m4',7),('cortex-m7',7),('cortex-m33',8)]:
 src=OUT/f'{cpu}.c';src.write_text(f'#if __TARGET_ARCH_M != {arch}\n#error incorrect CPU architecture\n#endif\nint entry(void){{unsigned x=123;return (x*7+5)^19;}}\nint asm_entry(void){{int x=35,y;asm volatile(\"add %0, %1, %2\" : \"=r\"(y) : \"r\"(x), \"i\"(7) : \"cc\");return y;}}\n')
 obj=OUT/f'{cpu}.o';exe=OUT/f'{cpu}.elf'
 run([WASM,'run','--dir=.','build/wasm/xhcc.wasm','arm-none-eabi-gcc','-mcpu='+cpu,'-mthumb','-c',src.relative_to(ROOT),'-o',obj.relative_to(ROOT)])
 run([ARM/'arm-none-eabi-ld.exe','-Ttext=0x8004000','-e','entry','-o',exe,obj])
 with exe.open('rb') as f:
  elf=ELFFile(f);entry=elf.get_section_by_name('.symtab').get_symbol_by_name('entry')[0]['st_value'];uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS);uc.mem_map(0x8000000,0x20000);uc.mem_map(0x20000000,0x20000)
  for sec in elf.iter_sections():
   if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
  uc.reg_write(UC_ARM_REG_SP,0x2001f000);uc.reg_write(UC_ARM_REG_LR,0x801f001);uc.reg_write(UC_ARM_REG_XPSR,0x1000000);uc.emu_start(entry|1,0x801f000,count=100000);got=uc.reg_read(UC_ARM_REG_R0);assert got==((123*7+5)^19),(cpu,got)
  asm_entry=elf.get_section_by_name('.symtab').get_symbol_by_name('asm_entry')[0]['st_value'];uc.reg_write(UC_ARM_REG_SP,0x2001f000);uc.reg_write(UC_ARM_REG_LR,0x801f001);uc.emu_start(asm_entry|1,0x801f000,count=100000);assert uc.reg_read(UC_ARM_REG_R0)==42
 print(f'PASS {cpu}: CPU macro, ELF GNU linking, Thumb execution={got}, bound asm=42')
