"""Package header regressions; use the lead's completed compiler build.

This suite does not rebuild compiler modules. XHCC_COMMAND may select the actual
production WASI compiler, as in test_driver.py. Emission checks establish target
compilation, not execution of ARM instructions.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_driver import ROOT, command


class McuHeaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.command = command()
        cls.workspace = tempfile.TemporaryDirectory(prefix='package-headers-', dir=ROOT)
        cls.directory = Path(cls.workspace.name)

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def invoke(self, source, *flags, target='arm-none-eabi-gcc'):
        src = self.directory / 'case.c'
        src.write_text(source, encoding='utf-8')
        out = self.directory / 'case.s'
        prefix = [] if any(Path(arg).name == 'xhcc.py' for arg in self.command) else ([target] if target else ['--target=c'])
        result = subprocess.run([*self.command, *prefix, *flags,
                                 src.relative_to(ROOT).as_posix()], cwd=ROOT,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout, out

    def test_assert_header_is_identical_across_packages(self):
        reference = (ROOT / 'packages/arm/include/assert.h').read_bytes()
        for package in ('riscv32', 'x86', 'x86_64', 'c'):
            self.assertEqual(reference, (ROOT / f'packages/{package}/include/assert.h').read_bytes())

    def test_ndebug_discards_side_effect_and_runtime_dependencies(self):
        source = '''
#include <assert.h>
int entry(void) {
    int side_effect = 0;
    assert(++side_effect);
    assert(this_identifier_must_not_reach_the_parser());
    return side_effect;
}
'''
        text, _ = self.invoke(source, '-E', '-DNDEBUG')
        self.assertNotIn('++side_effect', text)
        self.assertNotIn('this_identifier_must_not_reach_the_parser', text)
        self.assertNotIn('fprintf(', text)
        self.assertNotIn('abort(', text)
        _, out = self.invoke(source, '-S', '-DNDEBUG', '-O0', '-o',
                             (self.directory / 'case.s').relative_to(ROOT).as_posix())
        asm = out.read_text(encoding='utf-8')
        self.assertNotIn('abort', asm)
        self.assertNotIn('fprintf', asm)

    def test_enabled_assert_uses_existing_diagnostic_and_abort_abi(self):
        _, out = self.invoke('''
#include <assert.h>
int check_value(int value) { assert(value); return value; }
''', '-S', '-O0', '-o', (self.directory / 'case.s').relative_to(ROOT).as_posix())
        asm = out.read_text(encoding='utf-8')
        self.assertIn('fprintf', asm)
        self.assertIn('abort', asm)
        self.assertNotIn('__assert_fail', asm)
        self.assertIn('Assertion', asm)
        self.assertIn('check_value', asm)

    def test_assert_evaluates_expression_once_and_sizeof_does_not_call_it(self):
        text, _ = self.invoke('''
#include <assert.h>
int expression_once(void) { int n = 0; assert(++n); return n; }
int unevaluated(void) { int n = 0; return sizeof((assert(++n), n)); }
''', '-E')
        # Each expansion contains its operand once as code and once as a quoted
        # diagnostic. The string occurrence does not evaluate the operand.
        self.assertEqual(text.count('++n'), 4)
        _, out = self.invoke('''
#include <assert.h>
int unevaluated(void) { int n = 0; return sizeof((assert(++n), n)); }
''', '-S', '-O0', '-o', (self.directory / 'case.s').relative_to(ROOT).as_posix())
        asm = out.read_text(encoding='utf-8')
        self.assertNotIn('fprintf', asm)
        self.assertNotIn('abort', asm)

    def test_endianness_for_all_supported_targets(self):
        for target in ('arm-none-eabi-gcc', 'riscv32-unknown-elf-gcc',
                       'i686-linux-gnu-gcc', 'x86_64-linux-gnu-gcc', None):
            with self.subTest(target=target):
                self.invoke('''
#if __ORDER_LITTLE_ENDIAN__ != 1234 || __ORDER_BIG_ENDIAN__ != 4321 || __ORDER_PDP_ENDIAN__ != 3412
#error invalid compiler endian constants
#endif
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error byte order disagrees with supported little-endian descriptor
#endif
int endian_ok;
''', '-E', target=target)

    def test_endianness_preserves_user_override_order(self):
        self.invoke('''
#if __BYTE_ORDER__ != 73 || __ORDER_BIG_ENDIAN__ != 91
#error endian predefines overrode ordered user actions
#endif
''', '-E', '-U__BYTE_ORDER__', '-D__BYTE_ORDER__=73',
                    '-U__ORDER_BIG_ENDIAN__', '-D__ORDER_BIG_ENDIAN__=91')
        self.invoke('''
#if defined(__BYTE_ORDER__) || defined(__ORDER_PDP_ENDIAN__)
#error removed endian macros were reintroduced
#endif
''', '-E', '-D__BYTE_ORDER__=73', '-U__BYTE_ORDER__', '-U__ORDER_PDP_ENDIAN__')

    def test_reinclude_observes_changed_ndebug(self):
        text, _ = self.invoke('''
#define NDEBUG
#include <assert.h>
void disabled(void) { assert(disabled_expression_must_disappear); }
#undef NDEBUG
#include <assert.h>
void enabled(int value) { assert(value); }
#define NDEBUG
#include <assert.h>
void disabled_again(void) { assert(another_expression_must_disappear); }
''', '-E')
        self.assertNotIn('disabled_expression_must_disappear', text)
        self.assertNotIn('another_expression_must_disappear', text)
        self.assertIn('Assertion', text)


if __name__ == '__main__':
    unittest.main()
