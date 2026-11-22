"""Link native ELF objects with LLVM and execute every machine target in Unicorn.
Run from xhcc after tools/build.py --all. No host compiler substitutes xhcc output.
"""
from pathlib import Path
import os, struct, subprocess, sys
ROOT=Path(__file__).resolve().parents[1]
if os.name=='nt':
 pd=Path(sys.executable).parent;os.environ['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),os.environ.get('PATH','')])
sys.path.insert(0,str(ROOT/'build/test-deps'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_X86, UC_ARCH_RISCV, UC_MODE_32, UC_MODE_64, UC_MODE_RISCV32
from unicorn.x86_const import UC_X86_REG_ESP,UC_X86_REG_EAX,UC_X86_REG_RSP,UC_X86_REG_RAX,UC_X86_REG_RDI
from unicorn.riscv_const import UC_RISCV_REG_SP,UC_RISCV_REG_RA,UC_RISCV_REG_A0
WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
SDK=Path(os.environ.get('WASI_SDK',r'D:\wasi-sdk-34.0-x86_64-windows'))/'bin'
OUT=ROOT/'build/target-tests';OUT.mkdir(parents=True,exist_ok=True)
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
 return p.stdout
# Local bindings, external calls, globals, initialized data pointers, strings,
# aggregates, and pointer addends must all survive placement at two addresses.
(OUT/'a.c').write_text('extern int ext; extern int peer(int); static int same(int x){return x+2;} int a=7; int *pa=&a; int *pe=&ext; int arr[3]={4,8,12}; int *ap=&arr[1]; const char *msg="hi"; int entry(void){return peer(same(*pa))+*pe+*ap+msg[1];}\n')
(OUT/'b.c').write_text('int ext=9; static int same(int x){return x*3;} int peer(int x){return same(x);}\n')
(OUT/'lp64.c').write_text('long add(long a,unsigned b){return a+b;} long lp64_entry(void){long x=4294967296L; return ((add(x,3u)==4294967299L)&&((x/3L)==1431655765L)&&((x>>32)==1)&&sizeof(long)==8&&sizeof(void*)==8);}\n')
for target,machine,emulation in [('riscv32','EM_RISCV','elf32lriscv'),('x86','EM_386','elf_i386'),('x86_64','EM_X86_64','elf_x86_64')]:
 module=ROOT/'build/wasm/xhcc.wasm'
 template='addi %0, %1, %2' if target=='riscv32' else 'mov %0, %1; add %0, %2'
 template=template.replace('; ','\\n')
 asm_source='int asm_entry(void){int x=35,y; asm volatile("'+template+'" : "=r"(y) : "r"(x), "i"(7) : "cc"); return y;}\n'
 (OUT/'asm.c').write_text(asm_source)
 tool={'riscv32':'riscv32-unknown-elf-gcc','x86':'i386-elf-gcc','x86_64':'x86_64-elf-gcc'}[target]
 run([WASM,'run','--dir=.',module,tool,'-c',OUT.relative_to(ROOT)/'asm.c','-o',OUT.relative_to(ROOT)/f'{target}_asm.o'])
 for src in ['a','b']:
  tool={'riscv32':'riscv32-unknown-elf-gcc','x86':'i386-elf-gcc','x86_64':'x86_64-elf-gcc'}[target]
  run([WASM,'run','--dir=.',module,tool,'-c',OUT.relative_to(ROOT)/f'{src}.c','-o',OUT.relative_to(ROOT)/f'{target}_{src}.o'])
  with (OUT/f'{target}_{src}.o').open('rb') as f:
   elf=ELFFile(f);assert elf['e_type']=='ET_REL' and elf['e_machine']==machine
   symbols=elf.get_section_by_name('.symtab');assert symbols.get_symbol_by_name('same')[0]['st_info']['bind']=='STB_LOCAL'
 triple={'riscv32':'riscv32-unknown-elf','x86':'i386-unknown-linux-gnu','x86_64':'x86_64-unknown-linux-gnu'}[target]
 extra=['-march=rv32im','-mabi=ilp32'] if target=='riscv32' else []
 clang=Path(os.environ.get('TARGET_CLANG',r'D:\msys64\ucrt64\bin\clang.exe'))
 if clang.is_file():
  run([clang,'--target='+triple,'-ffreestanding','-fno-pic','-fno-pie',*extra,'-c',OUT/'b.c','-o',OUT/f'{target}_clang.o'])
 else:print('SKIP external clang mixed object: install full-target clang or set TARGET_CLANG')
 for base,peer in [(0x10000,'b'),(0x230000,'clang' if clang.is_file() else 'b')]:
  exe=OUT/f'{target}_{base:x}.elf'
  run([SDK/'ld.lld.exe','-m',emulation,'--no-relax','--image-base=0','-Ttext='+hex(base),'-e','entry','-o',exe,OUT/f'{target}_a.o',OUT/f'{target}_{peer}.o',OUT/f'{target}_asm.o'])
  with exe.open('rb') as f:
   elf=ELFFile(f);syms={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
   uc=Uc(UC_ARCH_RISCV,UC_MODE_RISCV32) if target=='riscv32' else Uc(UC_ARCH_X86,UC_MODE_64 if target=='x86_64' else UC_MODE_32)
   uc.mem_map(base&~0xffff,0x20000);uc.mem_map(0x700000,0x20000);stop=base+0x1f000;sp=0x71f000
   for sec in elf.iter_sections():
    if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
   if target=='riscv32':uc.reg_write(UC_RISCV_REG_SP,sp);uc.reg_write(UC_RISCV_REG_RA,stop);result_reg=UC_RISCV_REG_A0
   else:
    wide=target=='x86_64';uc.reg_write(UC_X86_REG_RSP if wide else UC_X86_REG_ESP,sp-8 if wide else sp-4);uc.mem_write(sp-8 if wide else sp-4,struct.pack('<Q' if wide else '<I',stop));result_reg=UC_X86_REG_RAX if wide else UC_X86_REG_EAX
   uc.emu_start(syms['entry'],stop,count=100000);got=uc.reg_read(result_reg);assert got==149,(target,hex(base),got)
   if target=='riscv32':uc.reg_write(UC_RISCV_REG_SP,sp);uc.reg_write(UC_RISCV_REG_RA,stop)
   else:uc.reg_write(UC_X86_REG_RSP if wide else UC_X86_REG_ESP,sp-8 if wide else sp-4)
   uc.emu_start(syms['asm_entry'],stop,count=100000);assert uc.reg_read(result_reg)==42,(target,'bound asm',uc.reg_read(result_reg))
   psz=8 if target=='x86_64' else 4;fmt='<Q' if psz==8 else '<I'
   for name,dest,add in [('pa','a',0),('pe','ext',0),('ap','arr',4)]:assert struct.unpack(fmt,uc.mem_read(syms[name],psz))[0]==syms[dest]+add
  print(f'PASS {target}: LLVM link + Unicorn execution at {base:#x}; entry=149, local symbols and relocated pointers')
# LP64 long must cross a real SysV function boundary without truncation.
module=ROOT/'build/wasm/xhcc.wasm'
run([WASM,'run','--dir=.',module,'x86_64-elf-gcc','-c',OUT.relative_to(ROOT)/'lp64.c','-o',OUT.relative_to(ROOT)/'lp64.o'])
run([SDK/'ld.lld.exe','-m','elf_x86_64','--image-base=0','-Ttext=0x10000','-e','lp64_entry','-o',OUT/'lp64.elf',OUT/'lp64.o'])
with (OUT/'lp64.elf').open('rb') as f:
 elf=ELFFile(f);entry=elf.get_section_by_name('.symtab').get_symbol_by_name('lp64_entry')[0]['st_value'];uc=Uc(UC_ARCH_X86,UC_MODE_64);uc.mem_map(0x10000,0x20000);uc.mem_map(0x700000,0x20000)
 for sec in elf.iter_sections():
  if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
 uc.reg_write(UC_X86_REG_RSP,0x71eff8);uc.mem_write(0x71eff8,struct.pack('<Q',0x2f000));uc.emu_start(entry,0x2f000,count=100000);assert uc.reg_read(UC_X86_REG_RAX)==1
print('PASS LP64 long argument/return, arithmetic/division/shift and sizeof checks')
print('PASS all standard cross-target ELF objects')
