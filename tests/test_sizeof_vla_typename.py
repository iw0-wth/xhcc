"""VLA sizeof type-name regressions; run after the coordinated compiler build.

Uses test_driver.command (XHCC_COMMAND override). This suite never rebuilds xhcc.
The supported new form has one outer variable bound and fixed-size elements;
VLA typedefs, inner variable dimensions and pointers to VLAs remain unsupported.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

from test_driver import ROOT, command

if os.name == 'nt':
    _python = Path(sys.executable).parent
    os.environ['PATH'] = os.pathsep.join(
        [str(_python), str(_python / 'Library/bin'), os.environ.get('PATH', '')])
sys.path.insert(0, str(ROOT / 'build/test-deps'))
GNU = Path(os.environ.get('GNUARM', r'D:\arm-none-eabi')) / 'bin'
EXE = '.exe' if os.name == 'nt' else ''


class SizeofVlaTypenameTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.command = command()
        cls.workspace = tempfile.TemporaryDirectory(prefix='sizeof-vla-', dir=ROOT)
        cls.addClassCleanup(cls.workspace.cleanup)
        cls.directory = Path(cls.workspace.name)
        cls.sequence = 0

    def run_checked(self, argv):
        result = subprocess.run(list(map(str, argv)), cwd=ROOT,
                                capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def compile(self, source, success=True, target='arm-none-eabi-gcc'):
        self.__class__.sequence += 1
        src = self.directory / f'case{self.sequence}.c'
        obj = src.with_suffix('.o')
        src.write_text(source, encoding='utf-8')
        args = [*self.command, target, '-std=c99', '-O0']
        if target == 'arm-none-eabi-gcc':
            args += ['-mcpu=cortex-m4']
        result = subprocess.run([*args, '-c', src.relative_to(ROOT).as_posix(),
                                 '-o', obj.relative_to(ROOT).as_posix()], cwd=ROOT,
                                capture_output=True, text=True, timeout=90)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse(obj.exists())
        return obj, result.stdout + result.stderr

    def execute(self, obj, definitions=''):
        from elftools.elf.elffile import ELFFile
        from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
        from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0
        objects = [obj]
        if definitions:
            src = obj.with_suffix('.gnu.c')
            gnu_obj = obj.with_suffix('.gnu.o')
            src.write_text(definitions, encoding='utf-8')
            self.run_checked([GNU / ('arm-none-eabi-gcc' + EXE), '-std=c99',
                              '-O0', '-mcpu=cortex-m4', '-mthumb', '-c', src,
                              '-o', gnu_obj])
            objects.append(gnu_obj)
        elf_path = obj.with_suffix('.elf')
        self.run_checked([GNU / ('arm-none-eabi-ld' + EXE), '-Ttext=0x10000',
                          '-Tdata=0x30000', '-e', 'entry', '-o', elf_path, *objects])
        with elf_path.open('rb') as stream:
            elf = ELFFile(stream)
            entry = elf.get_section_by_name('.symtab').get_symbol_by_name('entry')[0]
            uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
            uc.mem_map(0x10000, 0x40000)
            uc.mem_map(0x700000, 0x20000)
            for section in elf.iter_sections():
                if section['sh_flags'] & 2 and section['sh_type'] != 'SHT_NOBITS':
                    uc.mem_write(section['sh_addr'], section.data())
            uc.reg_write(UC_ARM_REG_SP, 0x71f000)
            uc.reg_write(UC_ARM_REG_LR, 0x4f001)
            uc.emu_start(entry['st_value'] | 1, 0x4f000, count=100000)
            return uc.reg_read(UC_ARM_REG_R0) & 0xffffffff

    def test_bound_evaluated_once_and_scaled_without_array_allocation(self):
        obj, _ = self.compile('''
int entry(void) {
    int n = 3;
    unsigned a = sizeof(int[n++]);
    unsigned b = sizeof(char[n++]);
    return a == 12 && b == 4 && n == 5;
}
''')
        self.assertEqual(self.execute(obj), 1)

    def test_extern_address_assertion_and_runtime_bound(self):
        obj, _ = self.compile('''
extern int a, b;
int entry(void) {
    ((void)sizeof(char[1 - 2 * !(&a != &b)]));
    return sizeof(char[1 - 2 * !(&a != &b)]);
}
''')
        self.assertEqual(self.execute(obj, 'int a, b;'), 1)
        # Different names can denote the same address across object files.
        self.assertEqual(self.execute(obj, '''
int a; extern int b __attribute__((alias("a")));
'''), 0xffffffff)

    def test_nested_sizeof_does_not_steal_local_vla_bound(self):
        obj, _ = self.compile('''
int entry(void) {
    int n = 3;
    return sizeof(char[sizeof(int[n++])]) == 12 && n == 4;
}
''')
        self.assertEqual(self.execute(obj), 1)

    def test_known_false_and_nonpositive_constant_bounds_rejected(self):
        for bound in ('-1', '0', '1 - 2 * !(&a != &a)'):
            with self.subTest(bound=bound):
                _, diagnostic = self.compile(
                    'extern int a; int entry(void) { return sizeof(char['
                    + bound + ']); }', success=False)
                self.assertIn('array dimension must be greater than zero', diagnostic)

    def test_file_scope_vla_and_noninteger_bound_rejected(self):
        self.compile('extern int a,b; char bad[1 - 2 * !(&a != &b)];', success=False)
        _, diagnostic = self.compile(
            'int entry(int *n) { return sizeof(char[n]); }', success=False)
        self.assertIn('bound must have integer type', diagnostic)

    def test_sizeof_result_has_target_unsigned_size_t_type(self):
        for target in ('arm-none-eabi-gcc', 'x86_64-linux-gnu-gcc'):
            with self.subTest(target=target):
                self.compile('''
int entry(int n) {
    return _Generic(sizeof(char[n]), unsigned int: sizeof(void*) == 4,
                    unsigned long: sizeof(unsigned long) == sizeof(void*),
                    unsigned long long: sizeof(unsigned long long) == sizeof(void*),
                    default: 0);
}
''', target=target)


if __name__ == '__main__':
    unittest.main()
