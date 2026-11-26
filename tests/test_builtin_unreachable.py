"""Reserved unreachable lowers to a local nonreturning loop, never a call."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from test_driver import ROOT, command
sys.path.insert(0, str(ROOT / 'build/test-deps'))

SOURCE = '''
int entry(int n) {
    if (n == 0) __builtin_unreachable();
    return n + 17;
}
int allocated(int n) {
    char *p = __builtin_alloca(8);
    if (n == 0) __builtin_unreachable();
    p[0] = 9;
    return p[0] + n;
}
'''

class UnreachableTests(unittest.TestCase):
    def invoke(self, args, success=True):
        p = subprocess.run([*command(), *map(str,args)], cwd=ROOT,
                           capture_output=True,text=True,timeout=90)
        self.assertEqual(p.returncode==0,success,p.stdout+p.stderr)
        return p

    def test_lowering_diagnostics_and_execution(self):
        from elftools.elf.elffile import ELFFile
        from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
        from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_PC
        gnu=Path(os.environ.get('GNUARM',r'D:\arm-none-eabi'))/'bin'
        with tempfile.TemporaryDirectory(prefix='unreachable-',dir=ROOT) as tmp:
            directory=Path(tmp);src=directory/'case.c';obj=directory/'case.o';asm=directory/'case.s';exe=directory/'case.elf'
            def rel(p):return p.relative_to(ROOT).as_posix()
            for invalid in ('__builtin_unreachable(1)','__builtin_unreachable(1,2)'):
                src.write_text('void bad(void){'+invalid+';}',encoding='utf-8')
                p=self.invoke(['arm-none-eabi-gcc','-mcpu=cortex-m3','-S',rel(src),'-o',rel(asm)],False)
                self.assertIn('requires exactly zero arguments',p.stderr)
            for opt in ('-O0','-O2'):
                # No allocation needed to verify target-neutral lowering.
                src.write_text(SOURCE.split('int allocated')[0],encoding='utf-8')
                for target in ('arm-none-eabi-gcc','riscv32-unknown-elf-gcc','i686-linux-gnu-gcc','x86_64-linux-gnu-gcc','--target=c'):
                    prefix=[target]+(['-mcpu=cortex-m3'] if target=='arm-none-eabi-gcc' else [])
                    self.invoke([*prefix,opt,'-fno-builtin','-S',rel(src),'-o',rel(asm)])
                    self.assertNotIn('__builtin_unreachable',asm.read_text(encoding='utf-8'))
                src.write_text(SOURCE,encoding='utf-8')
                self.invoke(['arm-none-eabi-gcc','-mcpu=cortex-m3',opt,'-fno-builtin','-c',rel(src),'-o',rel(obj)])
                p=subprocess.run(list(map(str,[gnu/'arm-none-eabi-ld.exe','-Ttext=0x10000','-e','entry',obj,'-o',exe])),capture_output=True,text=True)
                self.assertEqual(p.returncode,0,p.stdout+p.stderr)
                with exe.open('rb') as stream:
                    elf=ELFFile(stream);uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS);uc.mem_map(0x10000,0x40000);uc.mem_map(0x700000,0x4000)
                    for sec in elf.iter_sections():
                        if sec['sh_flags']&2 and sec['sh_type']!='SHT_NOBITS':uc.mem_write(sec['sh_addr'],sec.data())
                    syms=elf.get_section_by_name('.symtab');addresses={name:syms.get_symbol_by_name(name)[0]['st_value'] for name in ('entry','allocated')}
                for name,expected in [('entry',22),('allocated',14)]:
                    uc.reg_write(UC_ARM_REG_SP,0x703f00);uc.reg_write(UC_ARM_REG_LR,0x4f001);uc.reg_write(UC_ARM_REG_R0,5)
                    uc.emu_start(addresses[name]|1,0x4f000,count=100000)
                    self.assertEqual(uc.reg_read(UC_ARM_REG_R0),expected);self.assertEqual(uc.reg_read(UC_ARM_REG_SP),0x703f00)
                # This checks our selected implementation, not a C UB oracle:
                # bounded execution stays at one self-branch and does not return.
                uc.reg_write(UC_ARM_REG_SP,0x703f00);uc.reg_write(UC_ARM_REG_LR,0x4f001);uc.reg_write(UC_ARM_REG_R0,0)
                uc.emu_start(addresses['entry']|1,0x4f000,count=100)
                pc=uc.reg_read(UC_ARM_REG_PC)
                uc.emu_start(pc|1,0x4f000,count=100)
                self.assertEqual(uc.reg_read(UC_ARM_REG_PC),pc)
                self.assertNotEqual(pc,0x4f000)

if __name__=='__main__':unittest.main()
