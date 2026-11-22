"""Execute ARM custom-section data and constructor/destructor arrays after GNU linking."""
from pathlib import Path
import os,struct,subprocess,sys
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'build/test-deps'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_R0,UC_ARM_REG_XPSR
WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe');ARM=Path(r'D:\arm-none-eabi\bin');OUT=ROOT/'build/section-tests';OUT.mkdir(parents=True,exist_ok=True)
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
for mode in ['direct','listing']:
 obj=OUT/f'{mode}.o'
 if mode=='direct':run([WASM,'run','--dir=.','build/wasm/xhcc.wasm','-c','object/tests/sections.c','-o',obj.relative_to(ROOT)])
 else:
  run([WASM,'run','--dir=.','build/wasm/xhcc.wasm','-S','object/tests/sections.c','-o',(OUT/'listing.s').relative_to(ROOT)])
  run([ARM/'arm-none-eabi-gcc.exe','-mcpu=cortex-m3','-mthumb','-c',OUT/'listing.s','-o',obj])
 with obj.open('rb') as f:
  elf=ELFFile(f);assert elf.get_section_by_name('.custom')['sh_addralign']>=32
  assert elf.get_section_by_name('.init_array')['sh_type']=='SHT_INIT_ARRAY';assert elf.get_section_by_name('.fini_array')['sh_type']=='SHT_FINI_ARRAY'
 script=OUT/'layout.ld';script.write_text('SECTIONS { . = 0x8004000; .text : { *(.text*) } . = 0x20002000; .data : { *(.data*) } .custom : { *(.custom) } .init_array : { *(.init_array) } .fini_array : { *(.fini_array) } .bss : { *(.bss*) } }')
 exe=OUT/f'{mode}.elf';run([ARM/'arm-none-eabi-ld.exe','-T',script,'-e','section_entry','-o',exe,obj])
 with exe.open('rb') as f:
  elf=ELFFile(f);syms={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()};uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS);uc.mem_map(0x8000000,0x20000);uc.mem_map(0x20000000,0x20000)
  for sec in elf.iter_sections():
   if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
  def call(addr):
   uc.reg_write(UC_ARM_REG_SP,0x2001f000);uc.reg_write(UC_ARM_REG_LR,0x801f001);uc.reg_write(UC_ARM_REG_XPSR,0x1000000);uc.emu_start(addr|1,0x801f000,count=100000);return uc.reg_read(UC_ARM_REG_R0)
  init=struct.unpack('<I',elf.get_section_by_name('.init_array').data())[0];fini=struct.unpack('<I',elf.get_section_by_name('.fini_array').data())[0]
  assert init&1 and fini&1;call(init);assert call(syms['section_entry'])==33;call(fini);assert call(syms['section_entry'])==36
  assert struct.unpack('<I',uc.mem_read(syms['custom_ptr'],4))[0]==syms['custom'];assert syms['custom']%32==0
 print(f'PASS ARM {mode}: aligned custom data/pointer relocations, constructors/destructors executed via linked arrays')
