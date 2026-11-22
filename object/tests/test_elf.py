"""Real GNU mixed linking and Thumb execution. Requires unicorn/pyelftools.
Run from xhcc: python object/tests/test_elf.py.
Install dependencies: python -m pip install --target build/test-deps unicorn pyelftools.
"""
from pathlib import Path
import os, subprocess, sys, struct
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
sys.path.insert(0,str(ROOT/'build'/'test-deps'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_R0,UC_ARM_REG_XPSR
ARM=Path(os.environ.get('ARM_BIN',r'D:\arm-none-eabi\bin'))
WASMTIME=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
OUT=ROOT/'build'/'elf-tests';OUT.mkdir(parents=True,exist_ok=True)
def run(args):
    p=subprocess.run([str(x) for x in args],cwd=ROOT,capture_output=True,text=True)
    if p.returncode:raise RuntimeError(f'{args}\n{p.stdout}\n{p.stderr}')
    return p.stdout
run([WASMTIME,'run','--dir=.', 'build/wasm/xhcc.wasm','-c','object/tests/mixed.c','-o','build/elf-tests/mixed.o'])
run([ARM/'arm-none-eabi-gcc.exe','-mcpu=cortex-m3','-mthumb','-mfloat-abi=soft','-ffreestanding','-c',HERE/'gnu.c','-o',OUT/'gnu.o'])
with (OUT/'mixed.o').open('rb') as f:
    elf=ELFFile(f);assert elf.header.e_type=='ET_REL';assert elf.header.e_machine=='EM_ARM'
    syms=elf.get_section_by_name('.symtab');assert syms.get_symbol_by_name('local')[0]['st_info']['bind']=='STB_LOCAL'
    assert syms.get_symbol_by_name('callback')[0]['st_value']&1
    assert elf.get_section_by_name('.bss')['sh_type']=='SHT_NOBITS'
    reltypes={r['r_info_type'] for r in elf.get_section_by_name('.rel.text').iter_relocations()}
    assert {10,47,48}<=reltypes
run([WASMTIME,'run','--dir=.', 'build/wasm/xhcc.wasm','-c','object/tests/extended.c','-o','build/elf-tests/extended.o'])
for fixture in ['mixed','extended']:
    run([WASMTIME,'run','--dir=.', 'build/wasm/xhcc.wasm','-S',f'object/tests/{fixture}.c','-o',f'build/elf-tests/{fixture}.s'])
    run([ARM/'arm-none-eabi-gcc.exe','-mcpu=cortex-m3','-mthumb','-mfloat-abi=soft','-c',OUT/f'{fixture}.s','-o',OUT/f'{fixture}_listing.o'])
libgcc=run([ARM/'arm-none-eabi-gcc.exe','-mcpu=cortex-m3','-mthumb','-mfloat-abi=soft','-print-libgcc-file-name']).strip()
for suffix in ['', '_listing']:
    for text,data in [(0x08004000,0x20002000),(0x08120000,0x2000a000)]:
        script=OUT/'layout.ld';script.write_text(f'SECTIONS {{ . = 0x{text:x}; .text : {{ *(.text*) *(.rodata*) }} . = 0x{data:x}; .data : {{ *(.data*) }} .bss : {{ *(.bss*) *(COMMON) }} }}')
        exe=OUT/f'mixed{suffix}_{text:x}.elf'
        run([ARM/'arm-none-eabi-ld.exe','-T',script,'--entry=xh_entry','-o',exe,OUT/f'mixed{suffix}.o',OUT/f'extended{suffix}.o',OUT/'gnu.o',libgcc])
        with exe.open('rb') as f:
            elf=ELFFile(f);sym=elf.get_section_by_name('.symtab');vals={s.name:s['st_value'] for s in sym.iter_symbols()}
            uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS)
            uc.mem_map(text&~0xffff,0x20000);uc.mem_map(0x20000000,0x20000)
            for sec in elf.iter_sections():
                if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
            for name,want in [('xh_entry',115),('gnu_entry',13),('aggregate_entry',146),('soft_entry',7)]:
                uc.reg_write(UC_ARM_REG_SP,0x2001f000);uc.reg_write(UC_ARM_REG_LR,(text+0x1f000)|1);uc.reg_write(UC_ARM_REG_XPSR,0x01000000)
                uc.emu_start(vals[name]|1,text+0x1f000,count=100000)
                got=uc.reg_read(UC_ARM_REG_R0);assert got==want,(name,hex(text),got,want)
            ptr=struct.unpack('<I',uc.mem_read(vals['ptr'],4))[0];assert ptr==vals['external_data']
            fp=struct.unpack('<I',uc.mem_read(vals['fp'],4))[0];assert fp==vals['callback'] and fp&1
        print(f'PASS GNU mixed link and Cortex-M3 execution source={suffix or 'direct ELF'} text={text:#x} data={data:#x}: xh_entry=115 gnu_entry=13 aggregate_entry=146 soft_entry=7')
run([WASMTIME,'run','--dir=.', 'build/wasm/xhcc.wasm','--private-object','-c','object/tests/gnu.c','-o','build/elf-tests/private.ark_obj','--memory-report'])
assert (OUT/'private.ark_obj').read_bytes().startswith(b'ARKOBJ2\n')
bad=subprocess.run([WASMTIME,'run','--dir=.', 'build/wasm/xhcc.wasm','-c','object/tests/unsupported.c','-o','build/elf-tests/unsupported.o'],cwd=ROOT,capture_output=True,text=True)
assert bad.returncode and 'custom data sections' in bad.stderr,(bad.returncode,bad.stderr)
print('PASS ELF header, sections, Thumb bit, local binding, standard relocations, relocated data/function pointers, explicit private mode, unsupported section rejection')
