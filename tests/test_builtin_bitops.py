"""Reserved clz/expect: target-neutral lowering and GNU argument semantics.

Run with XHCC_COMMAND pointing to a freshly built compiler. Zero clz is only
an implementation regression check, not a GNU-defined-input oracle.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from test_driver import command
sys.path.insert(0, str(ROOT / 'build/test-deps'))

SOURCE = r'''
int first, second;
long exp_value(void) { ++first; return -17; }
long expected_value(void) { ++second; return 1; }
int clz(unsigned x) { return x + 9; }
int entry(void) {
    unsigned x = 1;
    int r = 7 + __builtin_clz(x++);
    if (r != 38 || x != 2) return 1;
    if (__builtin_clz((unsigned)0x80000000) != 0) return 2;
    if (__builtin_clz(1) != 31 || __builtin_clz(2) != 30) return 3;
    if (__builtin_clz(-1) != 0) return 4;
    if (__builtin_clz(0x100000001ULL) != 31) return 5;
    if (__builtin_clz(__builtin_clz(1)) != 27) return 6;
    if (__builtin_clz(0) != 32) return 7;
    if (__builtin_expect(exp_value(), expected_value()) != -17) return 8;
    if (first != 1 || second != 1) return 9;
    if (sizeof(__builtin_clz(++first)) != sizeof(int)) return 10;
    if (sizeof(__builtin_expect(++first, ++second)) != sizeof(long)) return 11;
    if (first != 1 || second != 1) return 12;
    if (!_Generic(__builtin_clz(1), int: 1, default: 0)) return 13;
    if (!_Generic(__builtin_expect(1, 0), long: 1, default: 0)) return 14;
    if (clz(1) != 10) return 15;
    if (__builtin_expect(0x100000001ULL, 0) != (long)0x100000001ULL) return 16;
    return 0;
}
'''


class BuiltinBitopsTests(unittest.TestCase):
    def invoke(self, args, success=True):
        args = [arg.relative_to(ROOT).as_posix() if isinstance(arg, Path) else str(arg)
                for arg in args]
        p = subprocess.run([*command(), *args], cwd=ROOT,
                           capture_output=True, text=True, timeout=90)
        self.assertEqual(p.returncode == 0, success, p.stdout + p.stderr)
        return p

    def test_diagnostics(self):
        invalid = (
            ('__builtin_clz()', 'exactly one argument'),
            ('__builtin_clz(1, 2)', 'exactly one argument'),
            ('sizeof(__builtin_clz())', 'exactly one argument'),
            ('__builtin_clz(1.5)', 'requires integer arguments'),
            ('sizeof(__builtin_clz((void *)0))', 'requires integer arguments'),
            ('__builtin_expect(1)', 'exactly two arguments'),
            ('__builtin_expect(1, 0, 2)', 'exactly two arguments'),
            ('sizeof(__builtin_expect(1))', 'exactly two arguments'),
            ('__builtin_expect(1, 1.5)', 'requires integer arguments'),
            ('sizeof(__builtin_expect((void *)0, 0))', 'requires integer arguments'),
        )
        with tempfile.TemporaryDirectory(prefix='bitops-diag-', dir=ROOT) as tmp:
            src = Path(tmp) / 'case.c'
            out = src.with_suffix('.s')
            for expression, diagnostic in invalid:
                with self.subTest(expression=expression):
                    src.write_text('int entry(void){' + expression + ';return 0;}', encoding='utf-8')
                    p = self.invoke(['arm-none-eabi-gcc', '-mcpu=cortex-m3', '-fno-builtin',
                                     '-S', src, '-o', out], False)
                    self.assertIn(diagnostic, p.stderr)

    def test_all_targets_no_external_builtin_calls(self):
        with tempfile.TemporaryDirectory(prefix='bitops-targets-', dir=ROOT) as tmp:
            src = Path(tmp) / 'case.c'
            out = src.with_suffix('.s')
            src.write_text(SOURCE, encoding='utf-8')
            for opt in ('-O0', '-O2'):
                for target in ('arm-none-eabi-gcc', 'riscv32-unknown-elf-gcc',
                               'i686-linux-gnu-gcc', 'x86_64-linux-gnu-gcc', '--target=c'):
                    with self.subTest(target=target, opt=opt):
                        cpu = ['-mcpu=cortex-m3'] if target == 'arm-none-eabi-gcc' else []
                        self.invoke([target, *cpu, opt, '-fno-builtin', '-S', src, '-o', out])
                        text = out.read_text(encoding='utf-8')
                        self.assertNotIn('__builtin_clz', text)
                        self.assertNotIn('__builtin_expect', text)

    def test_generated_c_execution(self):
        gcc = Path(os.environ.get('HOST_GCC', r'D:\msys64\ucrt64\bin\gcc.exe'))
        if not gcc.exists():
            self.skipTest('set HOST_GCC for generated C execution')
        env = os.environ.copy()
        env['PATH'] = str(gcc.parent) + os.pathsep + env.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='bitops-c-', dir=ROOT) as tmp:
            src = Path(tmp) / 'case.c'
            generated = Path(tmp) / 'generated.c'
            exe = Path(tmp) / 'case.exe'
            src.write_text(SOURCE + '\nint main(void) { return entry(); }\n', encoding='utf-8')
            for opt in ('-O0', '-O2'):
                self.invoke(['--target=c', opt, '-fno-builtin', '-S', src, '-o', generated])
                p = subprocess.run([str(gcc), '-std=c99', str(generated), '-o', str(exe)],
                                   capture_output=True, text=True, timeout=30, env=env)
                self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                p = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30, env=env)
                self.assertEqual(p.returncode, 0, p.stdout + p.stderr)

    def test_arm_execution_and_balanced_stack(self):
        from elftools.elf.elffile import ELFFile
        from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
        from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0
        gnu = Path(os.environ.get('GNUARM', r'D:\arm-none-eabi')) / 'bin'
        with tempfile.TemporaryDirectory(prefix='bitops-arm-', dir=ROOT) as tmp:
            directory = Path(tmp)
            src = directory / 'case.c'
            obj = directory / 'case.o'
            exe = directory / 'case.elf'
            src.write_text(SOURCE, encoding='utf-8')
            for opt in ('-O0', '-O2'):
                self.invoke(['arm-none-eabi-gcc', '-mcpu=cortex-m3', opt, '-fno-builtin',
                             '-c', src, '-o', obj])
                p = subprocess.run([str(gnu / 'arm-none-eabi-ld.exe'), '-Ttext=0x10000',
                                    '-e', 'entry', str(obj), '-o', str(exe)],
                                   capture_output=True, text=True, timeout=30)
                self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                with exe.open('rb') as stream:
                    elf = ELFFile(stream)
                    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
                    uc.mem_map(0x10000, 0x40000)
                    uc.mem_map(0x700000, 0x4000)
                    for section in elf.iter_sections():
                        if section['sh_flags'] & 2 and section['sh_type'] != 'SHT_NOBITS':
                            uc.mem_write(section['sh_addr'], section.data())
                    entry = elf.get_section_by_name('.symtab').get_symbol_by_name('entry')[0]['st_value']
                uc.reg_write(UC_ARM_REG_SP, 0x703f00)
                uc.reg_write(UC_ARM_REG_LR, 0x4f001)
                uc.emu_start(entry | 1, 0x4f000, count=1000000)
                self.assertEqual(uc.reg_read(UC_ARM_REG_R0), 0)
                self.assertEqual(uc.reg_read(UC_ARM_REG_SP), 0x703f00)


if __name__ == '__main__':
    unittest.main()
