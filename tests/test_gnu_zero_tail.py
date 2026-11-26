"""GNU [0] struct tails; requires a coordinated build, never rebuilds xhcc.

Uses XHCC_COMMAND through test_driver.command. Layout is compared with GNU ARM
under the real project's -std=c99 -Wall -Werror flags (no -pedantic).
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from test_driver import ROOT, command
import test_sizeof_vla_typename as vla_support

GNU, EXE = vla_support.GNU, vla_support.EXE


class GnuZeroTailTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.command = command()
        cls.workspace = tempfile.TemporaryDirectory(prefix='gnu-zero-', dir=ROOT)
        cls.addClassCleanup(cls.workspace.cleanup)
        cls.directory = Path(cls.workspace.name)
        cls.sequence = 0

    run_checked = vla_support.SizeofVlaTypenameTests.run_checked
    execute = vla_support.SizeofVlaTypenameTests.execute

    def compile(self, source, success=True):
        self.__class__.sequence += 1
        src = self.directory / f'case{self.sequence}.c'
        obj = src.with_suffix('.o')
        src.write_text(source if source.endswith('\n') else source + '\n', encoding='utf-8')
        result = subprocess.run(
            [*self.command, 'arm-none-eabi-gcc', '-std=c99', '-Wall', '-Werror',
             '-O0', '-mcpu=cortex-m4', '-c', src.relative_to(ROOT).as_posix(),
             '-o', obj.relative_to(ROOT).as_posix()], cwd=ROOT,
            capture_output=True, text=True, timeout=90)
        diagnostic = result.stdout + result.stderr
        if success:
            self.assertEqual(result.returncode, 0, diagnostic)
        else:
            self.assertNotEqual(result.returncode, 0, diagnostic)
            self.assertFalse(obj.exists())
        return src, obj, diagnostic

    def test_layout_sizeof_offset_alignment_stride_matches_gnu_arm(self):
        for declaration in (
            'struct S { char head; unsigned tail[0]; };',
            'struct S { char head; double tail[0]; };',
            'struct S { unsigned head; char c; short tail[0]; };',
            'struct S { char head; unsigned tail[0][3]; };',
            'struct S { char head; unsigned tail[0]; } __attribute__((packed));',
            '#pragma pack(push, 2)\nstruct S { char head; unsigned tail[0]; };\n#pragma pack(pop)',
            'struct E { short x; char y; }; struct S { char head; struct E tail[0]; };',
        ):
            with self.subTest(declaration=declaration):
                src, obj, _ = self.compile(declaration + '''
struct S objects[2];
unsigned entry(void) {
    return sizeof(struct S) + 32 * _Alignof(struct S)
        + 1024 * (unsigned)((char *)objects[0].tail - (char *)&objects[0])
        + 32768 * (unsigned)((char *)&objects[1] - (char *)&objects[0])
        + sizeof(objects[0].tail);
}
''')
                oracle = obj.with_suffix('.gnu.o')
                self.run_checked([GNU / ('arm-none-eabi-gcc' + EXE), '-std=c99',
                                  '-Wall', '-Werror', '-O0', '-mcpu=cortex-m4',
                                  '-mthumb', '-c', src, '-o', oracle])
                self.assertEqual(self.execute(obj), self.execute(oracle))

    def test_zero_tail_sizeof_is_constant_and_does_not_change_base_type(self):
        _, obj, _ = self.compile('''
typedef unsigned word;
struct S { char prefix; word state[1-1]; _Static_assert(1, "no member"); };
_Static_assert(sizeof(((struct S *)0)->state) == 0, "zero tail sizeof");
word ordinary[3];
int entry(void) {
    struct S s;
    return sizeof(s.state) == 0 && sizeof(ordinary) == 12 && sizeof(word) == 4;
}
''')
        self.assertEqual(self.execute(obj), 1)

    def test_micropython_code_state_shapes(self):
        _, obj, _ = self.compile('''
typedef void *mp_obj_t;
typedef unsigned mp_uint_t;
typedef struct _mp_code_state_t {
    void *fun_bc; const unsigned char *ip; mp_obj_t *sp;
    unsigned short n_state; unsigned short exc_sp_idx;
    void *old_globals; mp_obj_t state[0];
} mp_code_state_t;
typedef struct _mp_code_state_native_t {
    void *fun_bc; const unsigned char *ip; mp_obj_t *sp;
    unsigned short n_state; unsigned short exc_sp_idx;
    void *old_globals; mp_obj_t state[0];
} mp_code_state_native_t;
int entry(void) {
    return sizeof(mp_code_state_t) == 20 && sizeof(mp_code_state_native_t) == 20;
}
''')
        self.assertEqual(self.execute(obj), 1)

    def test_c_backend_preserves_explicit_zero_bound(self):
        src, _, _ = self.compile('''
struct S { char head; unsigned tail[0]; } value;
int entry(void) { return sizeof(value.tail) == 0 && sizeof(value) == 4; }
''')
        output = src.with_suffix('.generated.c')
        self.run_checked([*self.command, 'xh-c-gcc', '-std=c99', '-S',
                          src.relative_to(ROOT).as_posix(), '-o',
                          output.relative_to(ROOT).as_posix()])
        self.assertRegex(output.read_text(encoding='utf-8'), r'tail\s*\[\s*0\s*\]')
        oracle = src.with_suffix('.generated.o')
        # The C backend uses a zero-initialized interpreter stack. At -O1 GNU
        # removes its unused cells in this constant-only fixture, avoiding an
        # unrelated libc memset dependency in the freestanding execution harness.
        self.run_checked([GNU / ('arm-none-eabi-gcc' + EXE), '-std=c99', '-O1',
                          '-mcpu=cortex-m4', '-mthumb', '-Dark_entry=entry',
                          '-c', output, '-o', oracle])
        self.assertEqual(self.execute(oracle), 1)

    def test_negative_non_tail_union_and_inner_zero_rejected(self):
        cases = (
            'struct S { int head; int tail[-1]; };',
            'struct S { int tail[0]; int later; };',
            'struct S { int tail[0], later; };',
            'struct S { int tail[0]; struct { int later; }; };',
            'union S { int head; int tail[0]; };',
            'struct S { int head; int tail[2][0]; };',
            'struct S { int head; int (*tail)[0]; };',
            'struct S { int head; int tail[sizeof(char[0])]; };',
        )
        for source in cases:
            with self.subTest(source=source):
                _, _, diagnostic = self.compile(source, success=False)
                expected = ('array dimension must be greater than zero'
                            if '[-1]' in source or 'sizeof(char[0])' in source
                            else 'GNU zero-length array must be the outermost type of the last struct member')
                self.assertIn(expected, diagnostic)

    def test_local_global_typedef_and_sizeof_zero_bounds_still_rejected(self):
        for source in (
            'int bad[0];',
            'typedef int bad[0];',
            'int entry(void) { int bad[0]; return sizeof(bad); }',
            'int entry(void) { return sizeof(char[0]); }',
            'int entry(void) { return sizeof(char[-1]); }',
        ):
            with self.subTest(source=source):
                _, _, diagnostic = self.compile(source, success=False)
                self.assertIn('array dimension must be greater than zero', diagnostic)


if __name__ == '__main__':
    unittest.main()
