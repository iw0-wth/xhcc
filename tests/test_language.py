"""Language/IR regression tests against each built module (no build here).

Run from xhcc: python -m unittest discover -s tests -p test_language.py -v
XHCC_COMMAND accepts the same JSON argv override as test_driver.py.
XHCC_TEST_TARGET=arm|riscv32|x86|x86_64|c selects assembly expectations.
Compilation and emitted output establish coverage; these are not a C standard
conformance suite or execution tests of machine instructions on another ISA.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_driver import command, ROOT

TARGET = os.environ.get('XHCC_TEST_TARGET', 'arm')


class LanguageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.command = command()
        cls.workspace = tempfile.TemporaryDirectory(prefix='language-', dir=ROOT)
        cls.directory = Path(cls.workspace.name)

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def compile(self, source, *flags, success=True):
        src = self.directory / 'case.c'
        out = self.directory / 'case.s'
        src.write_text(source, encoding='utf-8')
        if out.exists():
            out.unlink()
        aliases = {'arm': 'arm-none-eabi-gcc', 'riscv32': 'riscv32-unknown-elf-gcc',
                   'x86': 'i686-linux-gnu-gcc', 'x86_64': 'x86_64-linux-gnu-gcc'}
        launcher_has_tool = any(Path(arg).name == 'xhcc.py' for arg in self.command)
        target_args = [] if launcher_has_tool else ([aliases[TARGET]] if TARGET in aliases else ['--target=c'])
        args = [*self.command, *target_args, '-S', *flags, '-o', out.relative_to(ROOT).as_posix(),
                src.relative_to(ROOT).as_posix()]
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=60)
        diagnostics = result.stdout + result.stderr
        if success:
            self.assertEqual(result.returncode, 0, diagnostics)
            self.assertTrue(out.exists(), diagnostics)
            return out.read_text(encoding='utf-8'), diagnostics
        self.assertNotEqual(result.returncode, 0, diagnostics)
        return '', diagnostics

    def test_c99_declarations_initializers_and_control(self):
        asm, _ = self.compile('''
struct Pair { int x; int y; };
int c99(int n) {
    int a[n];
    struct Pair p = {.y = 4, .x = 3};
    int *q = (int[]){1, 2, 3};
    int total = 0;
    for (int i = 0; i < n; ++i) { a[i] = i; total += a[i]; }
    return total + p.x + p.y + q[1];
}
''', '-std=c99', '-O0')
        self.assertIn('c99', asm)

    def test_c11_six_keyword_subset(self):
        # The six keywords are Alignas, Alignof, Static_assert, Generic,
        # Thread_local and Atomic. This intentionally does not claim full C11.
        asm, _ = self.compile('''
_Static_assert(_Alignof(int) >= 4, "int alignment");
_Alignas(8) static int aligned_value = 9;
static _Atomic(int) atomic_value = 4;
int c11_subset(void) {
    return _Generic(aligned_value, int: 11, default: 0)
           + atomic_value;
}
''', '-std=c11', '-O0')
        self.assertIn('c11_subset', asm)

    def test_c11_thread_local_has_explicit_target_boundary(self):
        source = 'static _Thread_local int tls_value = 2;\nint tls_read(void) { return tls_value; }\n'
        if TARGET == 'c':
            asm, _ = self.compile(source, '-std=c11', '-O0')
            self.assertIn('tls_read', asm)
            self.assertIn('__thread', asm)
        else:
            _, diagnostics = self.compile(source, '-std=c11', '-O0', success=False)
            self.assertIn('TLS', diagnostics)

    def test_c11_static_assert_is_checked(self):
        _, diagnostics = self.compile('_Static_assert(0, "expected failure");\n',
                                      '-std=c11', success=False)
        self.assertIn('assert', diagnostics.lower())

    def test_c11_generic_selection_output(self):
        asm, _ = self.compile('int generic_selected(void) { return '
                              '_Generic((int)0, int: 37, default: 91); }\n',
                              '-std=c11', '-O0')
        self.assertIn('37', asm)

    def test_long_long_variadic_and_function_pointer(self):
        asm, _ = self.compile('''
#include <stdarg.h>
static long long plus(long long x) { return x + 4294967296LL; }
int variadic(int n, ...) {
    va_list ap; va_start(ap, n); int v = va_arg(ap, int); va_end(ap); return v;
}
long long indirect(long long x) { long long (*p)(long long) = plus; return p(x); }
''', '-std=c99', '-O0')
        self.assertIn('indirect', asm)
        self.assertIn('variadic', asm)

    def test_basic_and_operand_free_extended_asm_output(self):
        asm, _ = self.compile('void asm_basic(void) { __asm__ volatile("nop"); '
                              '__asm__ volatile("nop" ::: "memory", "cc"); }\n', '-O0')
        self.assertGreaterEqual(asm.lower().count('nop'), 2)

    def test_extended_asm_operands_fail_honestly(self):
        for operands in [': "+r" (out)', ': "=&r" (out) : "r" (in)',
                         ': "=m" (out) : "r" (in)', ': : "r" (in + 1)']:
            with self.subTest(operands=operands):
                _, diagnostics = self.compile('int asm_operand(int in) { int out = 1; '
                                              '__asm__ volatile("nop" ' + operands + '); '
                                              'return out; }\n', success=False)
                self.assertIn('asm operands are not supported', diagnostics)

    def test_extended_asm_register_and_immediate_binding(self):
        templates = {'arm': 'ADD %0, %1, %2', 'riscv32': 'addi %0, %1, %2',
                     'x86': 'mov %0, %1; add %0, %2',
                     'x86_64': 'mov %0, %1; add %0, %2', 'c': 'nop'}
        source = ('int asm_bound(int in) { int out; __asm__ volatile("' +
                  templates[TARGET] + '" : "=r" (out) : "r" (in), "i" (3) '
                  ': "memory", "cc"); return out; }\n')
        if TARGET == 'c':
            _, diagnostics = self.compile(source, success=False)
            self.assertIn('asm operands are not supported', diagnostics)
        else:
            asm, _ = self.compile(source, '-O2', '-fopt-regalloc', '-fopt-vstack')
            self.assertNotIn('%0', asm)
            self.assertNotIn('%1', asm)
            self.assertIn('asm_bound', asm)
            self.assertIn('3', asm)
            if TARGET == 'arm':
                self.assertIn('ADD r0, r1, #3', asm)

    def test_register_clobber_fails_honestly(self):
        _, diagnostics = self.compile('void f(void) { __asm__ volatile("nop" ::: "r4"); }\n',
                                      success=False)
        self.assertIn('register clobbers are not supported', diagnostics)

    def test_armcc_asm_function_target(self):
        if TARGET in ('arm', 'c'):
            asm, _ = self.compile('__asm int raw_asm(void) { MOV r0, #17\n BX lr\n }\n', '-O0')
            self.assertIn('17', asm)
            self.assertIn('raw_asm', asm)
        else:
            _, diagnostics = self.compile('__asm int raw_asm(void) { MOV r0, #17\n BX lr\n }\n',
                                          success=False)
            self.assertIn('require the ARM target', diagnostics)

    def test_target_long_data_model(self):
        width = 8 if TARGET == 'x86_64' else 4
        asm, _ = self.compile('''
_Static_assert(sizeof(long) == WIDTH, "long width");
_Static_assert(_Alignof(long) == WIDTH, "long alignment");
_Static_assert(sizeof(struct { char c; long n; }) == 2 * WIDTH, "long layout");
int literal_rank(void) {
    return _Generic(4294967296L, long: 61, long long: 62, default: 63);
}
long wide_arithmetic(long a, unsigned int b) { return (a + b) / 3L; }
long long_identity(long a) { long local = a; return local; }
'''.replace('WIDTH', str(width)), '-std=c11', '-O0')
        self.assertIn('61' if width == 8 else '62', asm)
        self.assertIn('wide_arithmetic', asm)

    def test_no_builtin_preserves_declared_library_call(self):
        asm, _ = self.compile('int creal(int);\nint ordinary_call(void) { return creal(9); }\n',
                              '-fno-builtin', '-O0')
        if TARGET == 'c':
            self.assertIn('ark_creal(', asm)
        else:
            import re
            self.assertRegex(asm.lower(), r'(?:bl|call)\s+creal')

    def test_no_builtin_requires_library_declaration(self):
        _, diagnostics = self.compile('int ordinary_call(void) { return puts("hello"); }\n',
                                      '-fno-builtin', success=False)
        self.assertTrue('puts' in diagnostics or 'undefined' in diagnostics.lower(), diagnostics)

    def test_floating_class_beyond_legacy_argument_mask(self):
        parameters = ', '.join('int a' + str(i) for i in range(16))
        arguments = ', '.join(str(i + 1) for i in range(16))
        source = ('extern double late_fp(' + parameters + ', double last);\n'
                  'double call_late_fp(void) { return late_fp(' + arguments + ', 3.5); }\n')
        asm, _ = self.compile(source, '-O0')
        self.assertIn('late_fp', asm)
        if TARGET == 'x86_64':
            self.assertIn('xmm0', asm.lower())

    def test_reference_layout_and_bitfield_cases(self):
        # Representative cases from arkcc run_layout/run_bitfield_tests.
        asm, _ = self.compile('''
struct B { unsigned a:4; unsigned b:12; };
struct __attribute__((packed)) P { char c; int i; };
_Static_assert(sizeof(struct B) == 4, "bitfield container");
_Static_assert(sizeof(struct P) == 5, "packed layout");
int bitfield_ops(void) {
    struct B b = {1, 2}; b.a += 3; b.b++;
    return b.a * 25 + b.b * 5;
}
''', '-std=c11', '-O0')
        self.assertIn('bitfield_ops', asm)

    def test_reference_function_typedef_and_compound_literal(self):
        asm, _ = self.compile('''
typedef int binop(int, int);
int add(int a, int b) { return a + b; }
int apply(binop *f, int a, int b) { return f(a, b); }
struct Pair { int x; int y; };
struct Pair pair_value(void) { return (struct Pair){7, 9}; }
int typedef_call(void) { binop *p = add; return apply(p, 3, 4); }
''', '-std=c99', '-O0')
        self.assertIn('typedef_call', asm)
        self.assertIn('pair_value', asm)

    def test_atomic_rmw_rejected_instead_of_nonatomic_lowering(self):
        _, diagnostics = self.compile('static _Atomic(int) value;\n'
                                      'void increment(void) { value++; }\n',
                                      '-std=c11', success=False)
        self.assertIn('read-modify-write', diagnostics)

    def test_optional_ir_passes_accept_real_input(self):
        source = ('static const int value = 7; static int add(int x) { return x + value; }\n'
                  'int passes(int n) { int s = 0; for (int i = 0; i < 4; ++i) '
                  's += add(n); return s; }\n')
        for flags in [('-O2',), ('--autoinline',), ('--dce-cfg',), ('--licm',),
                      ('--unroll',), ('--tailmerge',), ('--xblock',),
                      ('--const-global',), ('--vectorize',)]:
            with self.subTest(flags=flags):
                asm, _ = self.compile(source, '-O2', *flags)
                self.assertIn('passes', asm)


if __name__ == '__main__':
    unittest.main()
