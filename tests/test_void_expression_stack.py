"""Void/comma expression stack balance, checked by ARM alloca CFG and CPU."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
from test_driver import ROOT, command
sys.path.insert(0, str(ROOT / 'build/test-deps'))

SOURCE = r'''
static int total;
void release(void *p) { total = total + *(unsigned char *)p; }
long long wide(void) { return 123; }
int entry(int n) {
    unsigned char *p = __builtin_alloca(16);
    int i, x = 0;
    p[0] = 3;
    total = 0;
    for (i = 0; i < n; i = i + 1) {
        ((void)n, release(p));
        (void)release(p);
        (void)((void)n, release(p));
        (release(p), release(p));
        (void)(x = x + 1);
        ((void)(x = x + 1), (void)release(p));
        (wide(), (void)0);
        (i & 1) ? (void)release(p) : (void)release(p);
    }
    return total + x;
}
'''

class VoidExpressionStackTests(unittest.TestCase):
    def run_cmd(self, args):
        p = subprocess.run(list(map(str, args)), cwd=ROOT, capture_output=True,
                           text=True, timeout=90)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return p

    def test_cfg_and_cpu(self):
        from elftools.elf.elffile import ELFFile
        from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
        from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0
        gnu = Path(os.environ.get('GNUARM', r'D:\arm-none-eabi')) / 'bin'
        with tempfile.TemporaryDirectory(prefix='void-stack-', dir=ROOT) as tmp:
            directory = Path(tmp)
            src = directory / 'case.c'
            src.write_text(SOURCE, encoding='utf-8')
            for opt in ('-O0', '-O2'):
                with self.subTest(opt=opt):
                    obj = directory / 'case.o'
                    exe = directory / 'case.elf'
                    self.run_cmd([*command(), 'arm-none-eabi-gcc', '-mcpu=cortex-m3',
                                  opt, '-c', src.relative_to(ROOT).as_posix(),
                                  '-o', obj.relative_to(ROOT).as_posix()])
                    self.run_cmd([gnu / 'arm-none-eabi-ld.exe', '-Ttext=0x10000',
                                  '-e', 'entry', obj, '-o', exe])
                    with exe.open('rb') as stream:
                        elf = ELFFile(stream)
                        uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
                        uc.mem_map(0x10000, 0x40000)
                        uc.mem_map(0x700000, 0x4000)
                        for sec in elf.iter_sections():
                            if sec['sh_flags'] & 2 and sec['sh_type'] != 'SHT_NOBITS':
                                uc.mem_write(sec['sh_addr'], sec.data())
                        address = elf.get_section_by_name('.symtab').get_symbol_by_name('entry')[0]['st_value']
                    uc.reg_write(UC_ARM_REG_SP, 0x703f00)
                    for n in (1, 41, 13):
                        before = uc.reg_read(UC_ARM_REG_SP)
                        uc.reg_write(UC_ARM_REG_LR, 0x4f001)
                        uc.reg_write(UC_ARM_REG_R0, n)
                        uc.emu_start(address | 1, 0x4f000, count=2000000)
                        self.assertEqual(uc.reg_read(UC_ARM_REG_R0), n * 23)
                        self.assertEqual(uc.reg_read(UC_ARM_REG_SP), before)

if __name__ == '__main__':
    unittest.main()
