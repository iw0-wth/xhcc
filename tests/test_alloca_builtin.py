"""Reserved alloca intrinsic semantics and the explicit execution guard.

Run after the lead's production compiler build. No compiler rebuild happens in
this suite. Dynamic-stack lowering is deliberately rejected until live operand
preservation and target ABI alignment have been verified.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_driver import ROOT, command


class AllocaBuiltinTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.command = command()
        cls.workspace = tempfile.TemporaryDirectory(prefix='alloca-', dir=ROOT)
        cls.addClassCleanup(cls.workspace.cleanup)
        cls.directory = Path(cls.workspace.name)
        cls.sequence = 0

    def compile(self, source, target='arm-none-eabi-gcc', extra=(), success=True):
        self.__class__.sequence += 1
        src = self.directory / f'case{self.sequence}.c'
        output = src.with_suffix('.o' if target else '.c.out')
        src.write_text(source, encoding='utf-8')
        prefix = [target] if target else ['--target=c']
        if target == 'arm-none-eabi-gcc':
            prefix += ['-mcpu=cortex-m3']
        p = subprocess.run([*self.command, *prefix, '-O0', *extra,
                            '-c' if target else '-S',
                            src.relative_to(ROOT).as_posix(), '-o',
                            output.relative_to(ROOT).as_posix()], cwd=ROOT,
                           capture_output=True, text=True, timeout=90)
        if success:
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        else:
            self.assertNotEqual(p.returncode, 0, p.stdout + p.stderr)
            self.assertFalse(output.exists(), 'failed intrinsic emitted an object')
        return p.stdout + p.stderr

    def test_headers_identical(self):
        reference = (ROOT / 'packages/arm/include/alloca.h').read_bytes()
        for package in ('riscv32', 'x86', 'x86_64', 'c'):
            self.assertEqual(reference, (ROOT / f'packages/{package}/include/alloca.h').read_bytes())

    def test_argument_count_diagnostics(self):
        for expression in ('__builtin_alloca()', '__builtin_alloca(4, 8)',
                           'sizeof(__builtin_alloca())'):
            with self.subTest(expression=expression):
                diagnostic = self.compile(f'int entry(void) {{ {expression}; return 0; }}',
                                          success=False)
                self.assertIn('__builtin_alloca requires exactly one argument', diagnostic)

    def test_noninteger_count_diagnostics(self):
        for expression in ('__builtin_alloca(1.5)', '__builtin_alloca((void *)0)',
                           'sizeof(__builtin_alloca(1.5))'):
            with self.subTest(expression=expression):
                diagnostic = self.compile(f'int entry(void) {{ {expression}; return 0; }}',
                                          success=False)
                self.assertIn('__builtin_alloca requires an integer byte count', diagnostic)

    def test_sizeof_result_is_void_pointer_without_evaluation(self):
        for target in ('arm-none-eabi-gcc', 'riscv32-unknown-elf-gcc',
                       'i686-linux-gnu-gcc', 'x86_64-linux-gnu-gcc', None):
            with self.subTest(target=target):
                self.compile('''
#include <alloca.h>
int entry(void) {
    int n = 0;
    return sizeof(alloca(++n)) == sizeof(void *) && n == 0
        && _Generic(__builtin_alloca(4), void *: 1, default: 0);
}
''', target=target, extra=('-fno-builtin',))

    def test_integer_counts_are_accepted_in_unevaluated_context(self):
        self.compile('''
int entry(int n, unsigned u, unsigned long long wide, char small) {
    return sizeof(__builtin_alloca(n)) + sizeof(__builtin_alloca(u))
         + sizeof(__builtin_alloca(wide)) + sizeof(__builtin_alloca(small));
}
''')

    def test_evaluated_calls_explicitly_rejected_on_every_target(self):
        for target in ('arm-none-eabi-gcc', 'riscv32-unknown-elf-gcc',
                       'i686-linux-gnu-gcc', 'x86_64-linux-gnu-gcc', None):
            for spelling in ('alloca', '__builtin_alloca'):
                with self.subTest(target=target, spelling=spelling):
                    diagnostic = self.compile(
                        '#include <alloca.h>\nint entry(int n) { void *p = '
                        + spelling + '(n); return p != 0; }', target=target,
                        extra=('-fno-builtin',), success=False)
                    self.assertIn('lowering requires verified dynamic-stack support', diagnostic)

    def test_live_operand_contexts_are_rejected(self):
        for expression in ('sink(alloca(n), 17)', '(p[0] = alloca(n), 0)'):
            with self.subTest(expression=expression):
                diagnostic = self.compile('''
#include <alloca.h>
int sink(void *p, int n) { return n; }
int entry(int n) { void *p[1]; return ''' + expression + '; }', success=False)
                self.assertIn('lowering requires verified dynamic-stack support', diagnostic)


if __name__ == '__main__':
    unittest.main()
